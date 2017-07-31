//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include "Learner.h"
#include "TensorView.h"
#include "Utils.h"
#include "Serialization.h"

#include <boost/range/adaptor/transformed.hpp>

#define DISPATCH_TO_TYPED_UPDATE_FUNCTION                                                                     \
    switch (smoothedGradientValue->GetDataType())                                                             \
    {                                                                                                         \
    case DataType::Float:                                                                                     \
        Update<float>(parameter, gradientValue, smoothedGradientValue, trainingSampleCount);                  \
        break;                                                                                                \
    case DataType::Double:                                                                                    \
        Update<double>(parameter, gradientValue, smoothedGradientValue, trainingSampleCount);                 \
        break;                                                                                                \
    default:                                                                                                  \
        NOT_IMPLEMENTED;                                                                                      \
    }

#define GET_WRITABLE_MATRICES                                                                                 \
    const auto& smoothedGradientMatrix = GetWritableMatrix<ElementType>(smoothedGradientValue);               \
    const auto& gradientMatrix = GetWritableMatrix<ElementType>(gradientValue);                               \
    const auto& parameterMatrix = GetWritableMatrix<ElementType>(parameter.Value());

using namespace Microsoft::MSR::CNTK;
using namespace std;

namespace CNTK
{
    LearningRateSchedule LearningRatePerSampleSchedule(std::vector<double> learning_rates)
    {
        auto range = learning_rates | boost::adaptors::transformed([](double r) {return RatePerSample(r); });
        return LearningRateSchedule(std::vector<Rate>(range.begin(), range.end()));
    }

    // This method completely replaces the current schedule with the new schedule. However, since
    // the new schedule starts at time 0 and the current time (in terms of the number of elapsed
    // samples or sweeps) t can be greater than 0, we need to adjust the new schedule by t time
    // units, so that it takes effect from the current point in time onwards.
    CNTK_API void Learner::ResetLearningRate(const LearningRateSchedule& learningRateSchedule)
    {
        m_learningRateSchedule.m_schedule.clear();
        m_learningRateSchedule.m_epochSize = learningRateSchedule.m_epochSize;

        // copy the new schedule over, adjusting for the current varlue of the corresponding unit
        // (samples or sweeps) count.
        auto currentCount = m_learningRateSchedule.IsSweepBased() ? m_sweepCount : m_sampleCount;
        for (const auto& kv : learningRateSchedule.m_schedule) 
        {
            m_learningRateSchedule.m_schedule[currentCount + kv.first] = kv.second;
        }
    }

    template <typename ElementType>
    /*static*/ shared_ptr<const Matrix<ElementType>> LearnerBase::GetMatrix(const NDArrayViewPtr& arrayView)
    {
        return arrayView->GetMatrix<ElementType>();
    }

    template <typename ElementType>
    /*static*/ shared_ptr<Matrix<ElementType>> LearnerBase::GetWritableMatrix(const NDArrayViewPtr& arrayView)
    {
        return arrayView->GetWritableMatrix<ElementType>();
    }

    template <typename ElementType>
    /*static*/ const TensorView<ElementType>* LearnerBase::GetTensorView(const NDArrayViewPtr& arrayView)
    {
        return arrayView->GetTensorView<ElementType>();
    }

    /*static*/ bool LearnerBase::HasNan(const NDArrayViewPtr& value, const char* name)
    {
        switch (value->GetDataType())
        {
        case DataType::Float:
            return value->GetMatrix<float>()->HasNan(name);
        case DataType::Double:
            return value->GetMatrix<double>()->HasNan(name);
        default:
            LogicError("Unsupported DataType %s", DataTypeName(value->GetDataType()));
        }
    }

    /*static*/ void LearnerBase::Print(const NDArrayViewPtr& value, const char* msg)
    {
        switch (value->GetDataType())
        {
        case DataType::Float:
            value->GetMatrix<float>()->Print(msg);
            break;
        case DataType::Double:
            value->GetMatrix<double>()->Print(msg);
            break;
        default:
            LogicError("Unsupported DataType %s", DataTypeName(value->GetDataType()));
        }
    }

    void LearnerBase::ResetSmoothedGradients()
    {
        for(auto v : m_smoothedGradientValues)
        {
            if (v.second->GetDataType() == DataType::Float)
                v.second->SetValue(0.0f);
            else if (v.second->GetDataType() == DataType::Double)
                v.second->SetValue(0.0);
            else
                LogicError("Unsupported DataType %s", DataTypeName(v.second->GetDataType()));
        }
    }

    // Clipping gradients to prevent outliers,
    template <typename ElementType>
    void LearnerBase::ClipGradient(Matrix<ElementType>& gradient, size_t actualMBSize) const
    {
        if (m_additionalOptions.gradientClippingThresholdPerSample != numeric_limits<double>::infinity())
        {
            double maxGradientPerMB = m_additionalOptions.gradientClippingThresholdPerSample * actualMBSize;
            if (m_additionalOptions.gradientClippingWithTruncation)
                gradient.InplaceTruncate(ElementType(maxGradientPerMB));
            else
            {
                // norm2 normalized
                double gradientNorm = gradient.FrobeniusNorm();
                if (gradientNorm > maxGradientPerMB)
                {
                    double normFactor = maxGradientPerMB / gradientNorm;
                    gradient *= ElementType(normFactor);
                }
            }
        }
    }

    // Performs additional preprocessing before calling the update method 
    // (gradient clipping and L2 regularization depending on the additional learning parameters).
    template <typename ElementType>
    void LearnerBase::PreProcess(const NDArrayViewPtr& parameterValue, const NDArrayViewPtr& gradientValue, size_t actualMBSize) const
    {
        const auto& gradientMatrix = gradientValue->GetWritableMatrix<ElementType>();

        // clipping gradients to prevent outliers
        ClipGradient<ElementType>(*gradientMatrix, actualMBSize);

        // L2 regularizer
        if (m_additionalOptions.l2RegularizationWeight > 0)
        {
            // multiply by actualMBSize so that it's invariant to minibatch size since learning rate is per sample
            const auto weight = m_additionalOptions.l2RegularizationWeight * actualMBSize;
            const auto& parameterMatrix = parameterValue->GetWritableMatrix<ElementType>();
            Matrix<ElementType>::ScaleAndAdd(ElementType(weight), *parameterMatrix, *gradientMatrix);
        }
    }

    // Performs additional postprocessing after the update method has been executed
    // (noise injection and L1 regularization specified by the additional learning parameters).
    template <typename ElementType>
    void LearnerBase::PostProcess(const Parameter& parameter, const NDArrayViewPtr& gradientValue, size_t actualMBSize) const
    {
        const auto& parameterValue = parameter.Value();
        const auto& parameterMatrix = parameterValue->GetWritableMatrix<ElementType>();
        const auto gaussianNoiseInjectionStdDev = GetCurrentTrainingParameterValue(m_additionalOptions.gaussianNoiseInjectionStdDev);
        if (gaussianNoiseInjectionStdDev > 0)
        {
            const auto& sgdUpdateNoise = Matrix<ElementType>::RandomGaussian(parameterMatrix->GetNumRows(), parameterMatrix->GetNumCols(),
                CPUDEVICE, ElementType(0.0), ElementType(gaussianNoiseInjectionStdDev), m_noiseInjectionSeed++);

            sgdUpdateNoise.TransferToDeviceIfNotThere(parameterMatrix->GetDeviceId(), true);

            Matrix<ElementType>::ScaleAndAdd(ElementType(1.0), sgdUpdateNoise, *parameterMatrix);
        }

        // L1 regularizer with proximal gradient descent method
        if (m_additionalOptions.l1RegularizationWeight > 0)
        {
            const auto learningRate = LearningRatePerSample(actualMBSize);
            // multiply by actualMBSize so that it's invariant to minibatch size since learning rate is per sample
            const auto weight = learningRate * m_additionalOptions.l1RegularizationWeight * actualMBSize;
            parameterValue->GetWritableMatrix<ElementType>()->InplaceSoftThreshold(ElementType(weight));
        }
    }

    template <typename ElementType>
    /*static*/ TensorView<ElementType>* LearnerBase::GetWritableTensorView(const NDArrayViewPtr& arrayView)
    {
        return arrayView->GetWritableTensorView<ElementType>();
    }

    LearnerBase::LearnerBase(const vector<Parameter>& parameters,
                             const LearningRateSchedule& learningRateSchedule,
                             AdditionalLearningOptions additionalOptions,
                             bool allocateSmoothGradients /* = true */)
                             : Learner(parameters, learningRateSchedule),
                             m_additionalOptions(additionalOptions), 
                             m_noiseInjectionSeed(Internal::GenerateRandomSeed())
    {
        if (parameters.empty())
            InvalidArgument("The parameters list specified to a Learner must not be empty.");

        std::unordered_set<Parameter> uniqueParameters(parameters.begin(), parameters.end());

        if (uniqueParameters.size() != parameters.size())
            InvalidArgument("Learner's parameters list must not contain duplicates.");

        if (allocateSmoothGradients)
        {
            for (const auto& parameter : parameters)
            {
                NDArrayViewPtr view = AllocateNDArrayView(parameter, parameter.Shape());
                m_smoothedGradientValues.emplace(parameter, view);
            }
        }
    }

    /*static*/ NDArrayViewPtr LearnerBase::AllocateNDArrayView(const Parameter& parameter, const NDShape& shape)
    {
        if (parameter.GetDataType() == DataType::Float)
        {
            return MakeSharedObject<NDArrayView>(float(0.0), shape, parameter.Value()->Device());
        }
        else
        {
            return MakeSharedObject<NDArrayView>(0.0, shape, parameter.Value()->Device());
        }
    }

    /*static*/ NDShape LearnerBase::GetMatrixShape(const Parameter& parameter)
    {
        if (parameter.GetDataType() == DataType::Float)
        {
            auto matrix = GetMatrix<float>(parameter.Value());
            return{ matrix->GetNumRows(), matrix->GetNumCols() };
        }
        else
        {
            auto matrix = GetMatrix<double>(parameter.Value());
            return{ matrix->GetNumRows(), matrix->GetNumCols() };
        }
    }

    /*virtual*/ bool LearnerBase::Update(unordered_map<Parameter, NDArrayViewPtr>& gradientValues, size_t trainingSampleCount, bool sweepEnd) /*override*/
    {
        ReportTrainingParameterValue(m_learningRateSchedule, L"Learning rate");

        if (Learner::LearningRatePerSample(trainingSampleCount) == 0.0)
        {
            return false;
        }

        // make sure trainingSampleCount is a valid value
        if (trainingSampleCount == 0)
            InvalidArgument("Learner::Update() cannot perform an update with an empty minibatch.");

        UpdateOnMinibatch(trainingSampleCount);

        for (const auto& parameter : Parameters())
        {
            const auto& smoothedGradientValue = m_smoothedGradientValues.at(parameter);
            const auto& gradientValue = gradientValues.at(parameter);
            // TODO: make this a runtime parameter.
#if DUMPOUTPUT
            LOGPRINTF(stderr, "Update_%ls\n", parameter.Uid().c_str());
#endif

#ifdef _DEBUG
            if (HasNan(smoothedGradientValue, "TrainOneEpoch/UpdateWeights/Learner::Update(): "))
                LogicError("%ls has NaNs in smoothedGradient.", parameter.Uid().c_str());
#endif

#if DUMPOUTPUT
            const auto learningRate = LearningRate(trainingSampleCount);
            const auto momentum = MomentumValueForMB(trainingSampleCount);
            LOGPRINTF(stderr, "learnRatePerSample=%0.8f, momentum=%0.8f, actualMBSize=%ld\n",
                      learningRate, momentum, trainingSampleCount);
            LOGPRINTF(stderr, "GradUpdateType()=%s, GradientUpdateNoiseStd()=%0.8f\n",
                      LearnerType().c_str(), m_additionalOptions.gaussianNoiseInjectionStdDev);
            Print(gradientValue, "Gradient Update");
            Print(smoothedGradientValue, "Smoothed Gradient Input");
#endif
            DISPATCH_TO_TYPED_UPDATE_FUNCTION;

#if DUMPOUTPUT
            Print(parameter.Value(), "Parameter Update");
#endif

#ifdef _DEBUG
            const auto& parameterValue = parameter.Value();
            if (HasNan(parameterValue, "TrainOneEpoch/UpdateWeights/Learner::Update(): "))
                LogicError("%ls has NaNs in parameter values after parameter update.", parameter.Uid().c_str());
#endif
        }
        m_sampleCount += trainingSampleCount;
        m_minibatchCount++;
        if (sweepEnd)
        {
            m_sweepCount++;
        }

        return true;
    }

    template <typename ElementType>
    void LearnerBase::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                             const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        const auto& parameterValue = parameter.Value();
        PreProcess<ElementType>(parameterValue, gradientValue, trainingSampleCount);
        Update(parameter, gradientValue, smoothedGradientValue, trainingSampleCount);
        PostProcess<ElementType>(parameter, gradientValue, trainingSampleCount);

        auto paramRef = parameter;
        paramRef.RecordValueUpdate();
    }

    string LearnerBase::LearnerType() const
    {
        return Typename(this);
    }

    static const std::wstring s_learnerTypeValue = L"Learner";

    /*virtual*/ Dictionary LearnerBase::CreateCheckpoint() /*override*/
    {
        Dictionary checkpoint;

        checkpoint[versionKey] = CurrentVersion();
        checkpoint[typeKey] = s_learnerTypeValue;
        checkpoint[sampleCountKey] = m_sampleCount;
        checkpoint[minibatchCountKey] = m_minibatchCount;
        checkpoint[learningRateScheduleKey] = m_learningRateSchedule.Serialize();
        checkpoint[noiseInjectionSeedKey] = m_noiseInjectionSeed;

        // TODO: should we also save momentum schedule into the checkpoint?
        // If that is the case, need to be able to override this method in subclasses.
        std::vector<DictionaryValue> serializedSmoothedGradients(Parameters().size());
        size_t i = 0;
        for (const auto& parameter : Parameters())
        {
            const auto& smoothedGradientValue = m_smoothedGradientValues.at(parameter);
            serializedSmoothedGradients[i++] = *smoothedGradientValue;
        }

        checkpoint[smoothedGradientsKey] = serializedSmoothedGradients;

        return checkpoint;
    }

    /*virtual*/ void LearnerBase::RestoreFromCheckpoint(const Dictionary& checkpoint) /*override*/
    {
        static const vector<std::wstring> s_requiredDictionaryKeys = { typeKey, sampleCountKey, minibatchCountKey, learningRateScheduleKey };

        auto version = ValidateDictionary<LearnerBase>(checkpoint, s_requiredDictionaryKeys, s_learnerTypeValue, CurrentVersion());

        if (version >= 2) 
        {
            ValidateDictionary<LearnerBase>(checkpoint, { smoothedGradientsKey }, s_learnerTypeValue, CurrentVersion());
        }

        m_sampleCount = checkpoint[sampleCountKey].Value<size_t>();
        m_minibatchCount = checkpoint[minibatchCountKey].Value<size_t>();

        if (checkpoint.Contains(noiseInjectionSeedKey)) 
        {
            m_noiseInjectionSeed = checkpoint[noiseInjectionSeedKey].Value<size_t>();
        }

        // TODO: which learning rate schedule should take precedence here? 
        // The one given at construction time or the one loaded from a checkpoint?
        m_learningRateSchedule = TrainingParameterSchedule<Rate>::Deserialize(checkpoint[learningRateScheduleKey].Value<Dictionary>());

        const auto& parameters = Parameters();

        auto getSmoothedGradValue = [version, &checkpoint] (size_t i, const Parameter& parameter) -> const DictionaryValue&
        {
            const auto& uid = parameter.Uid();

            if (version >= 2)
            {
                const auto& values = checkpoint[smoothedGradientsKey].Value<vector<DictionaryValue>>();
                
                if (values.size() <= i)
                    LogicError("Checkpoint does not contain smoothed gradient value for parameter '%S' (uid=%S).", 
                        parameter.AsString().c_str(), uid.c_str());
                

                return values.at(i);
            }
            
            if (!checkpoint.Contains(uid))
                LogicError("Checkpoint does not contain smoothed gradient value for parameter '%S' (uid=%S).", 
                    parameter.AsString().c_str(), uid.c_str());

            return checkpoint[uid];
        };

        for (auto i = 0; i < parameters.size(); i++)
        {
            const auto& parameter = parameters.at(i);
            const auto& uid = parameter.Uid();
            const NDArrayView& checkpointedValue = getSmoothedGradValue(i, parameter).Value<NDArrayView>();

            const auto& smoothedGradientValue = m_smoothedGradientValues.at(parameter);

            if (smoothedGradientValue->GetDataType() != checkpointedValue.GetDataType())
                LogicError("DataType of the smoothed gradient value restored from checkpoint for the parameter '%S' (uid = %ls) does not match the expected value.",
                            parameter.AsString().c_str(), uid.c_str());

            if (smoothedGradientValue->Shape() != checkpointedValue.Shape())
                LogicError("Shape '%S' of the smoothed gradient value restored from checkpoint for the parameter '%S' (uid = %ls) does not match the expected value.",
                           smoothedGradientValue->Shape().AsString().c_str(), parameter.AsString().c_str(),uid.c_str());

            smoothedGradientValue->CopyFrom(checkpointedValue);
        }
    }

    void LearnerBase::ReportTrainingParameterValue(const TrainingParameterSchedule<Rate>& schedule, const wstring& name) const
    {
        auto value = GetCurrentTrainingParameterValue(schedule);

        auto iter = m_trainingParametersMap.find(name);
        //TODO: make m_trainingParametersMap track non-double parameters
        if (iter == m_trainingParametersMap.end() || iter->second != value.Value())
        {
            m_trainingParametersMap[name] = value.Value();

            wstringstream stream;
            stream << name;
            stream << L" [reference mbsize = " << value.ReferenceMBSize() << "]";
            wstring prefix = stream.str();

            for (auto& writer : m_progressWriters)
                writer->Write(prefix, value.Value());
        }
    }

    LearnerSGD::LearnerSGD(const std::vector<Parameter>& parameters, 
                           const LearningRateSchedule& learningRateSchedule, 
                           AdditionalLearningOptions additionalOptions,
                           bool allocateSmoothGradients)
                           : LearnerBase(parameters, learningRateSchedule, additionalOptions, allocateSmoothGradients)
    {
        if (!allocateSmoothGradients)
        {
            // the vanilla sgd does not need the smooth gradients per se, 
            // insert dummy nd views instead.
            for (const auto& parameter : parameters)
            {
                m_smoothedGradientValues.emplace(parameter, AllocateNDArrayView(parameter, {}));
            }
        }
    }

    /*virtual*/ void LearnerSGD::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerSGD::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                            const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        UNUSED(smoothedGradientValue);
        const auto& gradientMatrix = GetWritableMatrix<ElementType>(gradientValue);
        const auto& parameterMatrix = GetWritableMatrix<ElementType>(parameter.Value());
        const auto learningRate = ElementType(LearningRatePerSample(trainingSampleCount));

        parameterMatrix->SGDUpdate(*gradientMatrix, learningRate);
    }

    double LearnerMomentumSGD::MomentumValueForMB(const MomentumSchedule& schedule, size_t minibatchSize) const
    {
        auto currentMomentum = GetCurrentTrainingParameterValue(schedule);
        return Learner::ExponetialDecayRateForMinibatch(currentMomentum, minibatchSize);
    }

    /*virtual*/ void LearnerMomentumSGD::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                                const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        ReportTrainingParameterValue(m_momentumSchedule, L"Momentum");

        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerMomentumSGD::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                    const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES;

        const auto learningRate = ElementType(LearningRatePerSample(trainingSampleCount));
        const auto momentum = ElementType(MomentumValueForMB(trainingSampleCount));

        parameterMatrix->MomentumSGDUpdate(*gradientMatrix, *smoothedGradientMatrix,
                                           learningRate, momentum, UseUnitGainMomentum());
    }

    /*virtual*/ void LearnerNesterov::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                             const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerNesterov::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                 const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES;

        const auto learningRate = ElementType(LearningRatePerSample(trainingSampleCount));
        const auto momentum = ElementType(MomentumValueForMB(trainingSampleCount));

        parameterMatrix->NesterovAcceleratedMomentumSGDUpdate(*gradientMatrix, *smoothedGradientMatrix,
                                                              learningRate, momentum, UseUnitGainMomentum());
    }

    LearnerAdaGrad::LearnerAdaGrad(const std::vector<Parameter>& parameters,
                                   const LearningRateSchedule& learningRateSchedule,
                                   bool needAveMultiplier,
                                   AdditionalLearningOptions additionalOptions)
                                   : LearnerBase(parameters, learningRateSchedule, additionalOptions, /*allocateSmoothGradients*/ false),
                                   m_needAveMultiplier(needAveMultiplier)
    {
        for (const auto& parameter : parameters)
        {
            // When needAveMultiplier == true, CPU and GPU implementations of LearnerAdaGrad require different number of columns.
            size_t factor = 1;
            if (needAveMultiplier && parameter.Value()->Device().Type() == DeviceKind::GPU)
            {
                factor = 2;
            }

            const auto shape = GetMatrixShape(parameter);
            NDArrayViewPtr view = AllocateNDArrayView(parameter, { shape[0], factor * shape[1] });

            m_smoothedGradientValues.emplace(parameter, view);
        }
    }

    /*virtual*/ void LearnerAdaGrad::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                            const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerAdaGrad::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES

        const auto learningRate = LearningRatePerSample(trainingSampleCount);

        const auto aveMultiplier = smoothedGradientMatrix->Adagrad(*gradientMatrix, m_needAveMultiplier);
        Matrix<ElementType>::ScaleAndAdd(ElementType(-learningRate / aveMultiplier), *gradientMatrix, *parameterMatrix);
    }

    LearnerAdaDelta::LearnerAdaDelta(
        const std::vector<Parameter>& parameters,
        const LearningRateSchedule& learningRateSchedule,
        double rho, double epsilon,
        AdditionalLearningOptions additionalOptions)
        : LearnerBase(parameters, learningRateSchedule, additionalOptions, /*allocateSmoothGradients*/ false),
        m_rho(rho), m_epsilon(epsilon)
    {
        for (const auto& parameter : parameters)
        {
            const auto shape = GetMatrixShape(parameter);
            NDArrayViewPtr view = AllocateNDArrayView(parameter, { shape[0], 2 * shape[1] });
            m_smoothedGradientValues.emplace(parameter, view);
        }
    }

    /*virtual*/ void LearnerAdaDelta::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue,
        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerAdaDelta::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue,
        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES

        const auto learningRate = LearningRatePerSample(trainingSampleCount);

        smoothedGradientMatrix->AdaDeltaUpdate(*gradientMatrix, *parameterMatrix, (ElementType)learningRate, (ElementType)m_rho, (ElementType)m_epsilon);
    }

    /*static*/ const double LearnerFSAdaGrad::s_targetAdagradAvDenom = 1.0;

    LearnerFSAdaGrad::LearnerFSAdaGrad(const vector<Parameter>& parameters,
                                       const LearningRateSchedule& learningRateSchedule,
                                       const MomentumSchedule& momentumSchedule,
                                       bool unitGain,
                                       const MomentumSchedule& varianceMomentumSchedule,
                                       AdditionalLearningOptions additionalOptions)
                                       : LearnerMomentumSGD(parameters, learningRateSchedule, momentumSchedule, 
                                                            unitGain, additionalOptions, /*allocateSmoothGradients*/ false),
                                       m_varianceMomentumSchedule(varianceMomentumSchedule),
                                       m_smoothedCount(0.0)
    {
        for (const auto& parameter : parameters)
        {
            const auto shape = GetMatrixShape(parameter);
            NDArrayViewPtr view = AllocateNDArrayView(parameter, { shape[0], 2 * shape[1] });
            m_smoothedGradientValues.emplace(parameter, view);
        }
    }

    /*virtual*/ Dictionary LearnerFSAdaGrad::CreateCheckpoint() /*override*/
    {
        auto dict = LearnerBase::CreateCheckpoint();
        dict[smoothedCountKey] = m_smoothedCount;
        return dict;
    }

    /*virtual*/ void LearnerFSAdaGrad::RestoreFromCheckpoint(const Dictionary& checkpoint) /*override*/
    {
        LearnerBase::RestoreFromCheckpoint(checkpoint);
        m_smoothedCount = checkpoint[smoothedCountKey].Value<double>();
    }

    /*virtual*/ void LearnerFSAdaGrad::ResetSmoothedGradients() /*override*/
    {
        LearnerBase::ResetSmoothedGradients();
        m_smoothedCount = 0.0;
    }

    /*virtual*/ void LearnerFSAdaGrad::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                              const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    /*virtual*/ void LearnerFSAdaGrad::UpdateOnMinibatch(size_t trainingSampleCount)
    {
        const auto varMomentum = VarianceMomentumValueForMB(trainingSampleCount);

        // keep track on how many samples have been accumulated into the g^2 accumulator
        m_smoothedCount = varMomentum * m_smoothedCount + (1.0 - varMomentum) * trainingSampleCount;

        // update the numerator and then do the meanMomentum-based model update
        // Each AdaGrad-normalized gradient value is multiplied by the following, which
        //  - makes up for general scaling (targetAdagradAvDenom, a constant chosen by the user that should resemble the typical value range of gradients)
        //  - sqrt(1/#samples accumulated) to turn the sqr sum into an average
        m_targetAdagradAvDenom_x_sqrtAdagradSqrFrames = s_targetAdagradAvDenom * sqrt(m_smoothedCount);
    }

    template <typename ElementType>
    void LearnerFSAdaGrad::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                  const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES;

        const auto learningRate = LearningRatePerSample(trainingSampleCount);
        const auto momentum = MomentumValueForMB(trainingSampleCount);
        const auto varMomentum = VarianceMomentumValueForMB(trainingSampleCount);

        smoothedGradientMatrix->FSAdagradUpdate(*gradientMatrix, *parameterMatrix, m_targetAdagradAvDenom_x_sqrtAdagradSqrFrames, learningRate,
                                                momentum, varMomentum, UseUnitGainMomentum());
    }

    LearnerAdam::LearnerAdam(const vector<Parameter>& parameters,
        const LearningRateSchedule& learningRateSchedule,
        const MomentumSchedule& momentumSchedule,
        bool unitGain,
        const MomentumSchedule& varianceMomentumSchedule,
        double epsilon,
        bool adamax,
        AdditionalLearningOptions additionalOptions)
        : LearnerMomentumSGD(parameters, learningRateSchedule, momentumSchedule,
            unitGain, additionalOptions, /*allocateSmoothGradients*/ false),
          m_varianceMomentumSchedule(varianceMomentumSchedule), m_epsilon(epsilon),
          m_adamax(adamax)
    {

        if (m_epsilon < 0.0)
        {
            InvalidArgument("Epsilon should be non-negative. You are trying to set it to %g.", m_epsilon);
        }

        for (const auto& parameter : parameters)
        {
            const auto shape = GetMatrixShape(parameter);
            NDArrayViewPtr view = AllocateNDArrayView(parameter, {shape[0], 2 * shape[1]});
            m_smoothedGradientValues.emplace(parameter, view);
        }
        m_smoothedCount = 0.0;
    }

    /*virtual*/ Dictionary LearnerAdam::CreateCheckpoint() /*override*/
    {
        auto dict = LearnerBase::CreateCheckpoint();
        dict[smoothedCountKey] = m_smoothedCount;
        return dict;
    }

    /*virtual*/ void LearnerAdam::RestoreFromCheckpoint(const Dictionary& checkpoint) /*override*/
    {
        LearnerBase::RestoreFromCheckpoint(checkpoint);
        m_smoothedCount = checkpoint[smoothedCountKey].Value<double>();
    }

    /*virtual*/ void LearnerAdam::ResetSmoothedGradients() /*override*/
    {
        LearnerBase::ResetSmoothedGradients();
        m_smoothedCount = 0.0;
    }

    /*virtual*/ void LearnerAdam::UpdateOnMinibatch(size_t trainingSampleCount)
    {
        m_smoothedCount += 1.0;
    }

    /*virtual*/ void LearnerAdam::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue,
        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    template <typename ElementType>
    void LearnerAdam::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue,
        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES;

        const auto learningRate = LearningRatePerSample(trainingSampleCount);
        const auto momentum = MomentumValueForMB(trainingSampleCount);

        const auto varMomentum = VarianceMomentumValueForMB(trainingSampleCount);

        smoothedGradientMatrix->AdamUpdate(*gradientMatrix, *parameterMatrix, m_smoothedCount, learningRate,
                                           momentum, varMomentum, (ElementType)m_epsilon, UseUnitGainMomentum(), m_adamax);
    }

    LearnerRMSProp::LearnerRMSProp(const vector<Parameter>& parameters,
                                   const LearningRateSchedule& learningRateSchedule,
                                   double gamma, double inc, double dec, double max, double min,
                                   bool needAveMultiplier,
                                   AdditionalLearningOptions additionalOptions)
                                   : LearnerBase(parameters, learningRateSchedule, additionalOptions, /*allocateSmoothGradients*/ false),
                                   m_gamma(gamma), m_inc(inc), m_dec(dec), m_max(max), m_min(min), m_needAveMultiplier(needAveMultiplier)
    {
        // validation of learner settings
        if (gamma <= 0 || gamma >= 1)
            LogicError("RMSProp gamma must be in range (0.0, 1.0)");

        if (inc <= 1.0)
            LogicError("RMSProp inc must be greater than 1");

        if (dec <= 0 || dec >= 1)
            LogicError("RMSProp dec must be in range (0.0, 1.0)");

        if (max <= 0 || max <= min)
            LogicError("RMSProp max must be greater than zero and greater than min");

        if (min <= 0)
            LogicError("RMSProp min must be greater than zero");

        for (const auto& parameter : parameters)
        {
            // When needAveMultiplier == true, CPU and GPU implementations of RMSProp require different number of columns.
            size_t factor = 3;
            if (needAveMultiplier && parameter.Value()->Device().Type() == DeviceKind::GPU)
            {
                factor = 4;
            }

            const auto shape = GetMatrixShape(parameter);
            NDArrayViewPtr view = AllocateNDArrayView(parameter, { shape[0], factor * shape[1] });

            m_smoothedGradientValues.emplace(parameter, view);
        }
        m_smoothedCount = 0.0;
    }

    /*virtual*/ void LearnerRMSProp::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                            const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        DISPATCH_TO_TYPED_UPDATE_FUNCTION;
    }

    /*virtual*/ Dictionary LearnerRMSProp::CreateCheckpoint() /*override*/
    {
        auto dict = LearnerBase::CreateCheckpoint();
        dict[smoothedCountKey] = m_smoothedCount;
        return dict;
    }

    /*virtual*/ void LearnerRMSProp::RestoreFromCheckpoint(const Dictionary& checkpoint) /*override*/
    {
        LearnerBase::RestoreFromCheckpoint(checkpoint);
        m_smoothedCount = checkpoint[smoothedCountKey].Value<double>();
    }

    /*virtual*/ void LearnerRMSProp::ResetSmoothedGradients() /*override*/
    {
        LearnerBase::ResetSmoothedGradients();
        m_smoothedCount = 0.0;
    }

    /*virtual*/ void LearnerRMSProp::UpdateOnMinibatch(size_t trainingSampleCount)
    {
        m_smoothedCount += 1.0;
    }

    template <typename ElementType>
    void LearnerRMSProp::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue, 
                                const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const
    {
        GET_WRITABLE_MATRICES;

        const auto learningRate = LearningRatePerSample(trainingSampleCount);

        const auto aveMultiplier = smoothedGradientMatrix->RmsProp(*gradientMatrix,
                                                                   ElementType(m_gamma),
                                                                   ElementType(m_inc),
                                                                   ElementType(m_max),
                                                                   ElementType(m_dec),
                                                                   ElementType(m_min),
                                                                   m_needAveMultiplier,
                                                                   m_smoothedCount > 1);

        Matrix<ElementType>::ScaleAndAdd(ElementType(-learningRate / aveMultiplier), *gradientMatrix, *parameterMatrix);
    }

    // Explicit template instantiations
    template shared_ptr<Matrix<float>> LearnerBase::GetWritableMatrix<float>(const NDArrayViewPtr& arrayView);
    template shared_ptr<Matrix<double>> LearnerBase::GetWritableMatrix<double>(const NDArrayViewPtr& arrayView);

    LearnerPtr SGDLearner(const vector<Parameter>& parameters,
                          const LearningRateSchedule& learningRateSchedule,
                          AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerSGD>(parameters, learningRateSchedule, additionalOptions);
    }

    LearnerPtr MomentumSGDLearner(const vector<Parameter>& parameters,
                                  const LearningRateSchedule& learningRateSchedule,
                                  const MomentumSchedule& momentumSchedule,
                                  bool unitGain,
                                  AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerMomentumSGD>(parameters, learningRateSchedule, momentumSchedule, unitGain, additionalOptions);
    }

    LearnerPtr NesterovLearner(const vector<Parameter>& parameters,
                               const LearningRateSchedule& learningRateSchedule,
                               const MomentumSchedule& momentumSchedule,
                               bool unitGain,
                               AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerNesterov>(parameters, learningRateSchedule, momentumSchedule, unitGain, additionalOptions);
    }

    LearnerPtr FSAdaGradLearner(const vector<Parameter>& parameters,
                                const LearningRateSchedule& learningRateSchedule,
                                const MomentumSchedule& momentumSchedule,
                                bool unitGain, /*=true*/
                                const MomentumSchedule& varianceMomentumSchedule, /*= MomentumAsTimeConstantSchedulePerSample(2 * 3600 * 100)*/
                                AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerFSAdaGrad>(parameters, learningRateSchedule, momentumSchedule, unitGain, varianceMomentumSchedule, additionalOptions);
    }

    LearnerPtr AdamLearner(const vector<Parameter>& parameters,
                           const LearningRateSchedule& learningRateSchedule,
                           const MomentumSchedule& momentumSchedule,
                           bool unitGain, /*=true*/
                           const MomentumSchedule& varianceMomentumSchedule, /*= MomentumAsTimeConstantSchedulePerSample(2 * 3600 * 100)*/
                           double epsilon,
                           bool adamax, /*=false*/
                           AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerAdam>(parameters, learningRateSchedule, momentumSchedule, unitGain, varianceMomentumSchedule, epsilon, adamax, additionalOptions);
    }

    LearnerPtr AdaGradLearner(const vector<Parameter>& parameters,
                              const LearningRateSchedule& learningRateSchedule,
                              bool needAveMultiplier /*= true*/,
                              AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerAdaGrad>(parameters, learningRateSchedule, needAveMultiplier, additionalOptions);
    }

    LearnerPtr RMSPropLearner(const vector<Parameter>& parameters,
                              const LearningRateSchedule& learningRateSchedule,
                              double gamma, double inc, double dec, double max, double min,
                              bool needAveMultiplier /*= true*/,
                              AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerRMSProp>(parameters, learningRateSchedule, gamma, inc, dec, max, min, needAveMultiplier, additionalOptions);
    }

    LearnerPtr AdaDeltaLearner(const vector<Parameter>& parameters,
                               const LearningRateSchedule& learningRateSchedule,
                               double rho, double epsilon,
                               AdditionalLearningOptions additionalOptions /*= AdditionalLearningOptions()*/)
    {
        return MakeSharedObject<LearnerAdaDelta>(parameters, learningRateSchedule, rho, epsilon, additionalOptions);
    }

    


    LearnerUniversal::LearnerUniversal(const std::vector<Parameter>& parameters, const ParameterUpdateFunctor& func)
        : LearnerBase(parameters, LearningRateSchedule(Rate(1.0, 1)), AdditionalLearningOptions(), /*allocateSmoothGradients*/ false)
    {
        std::vector<Variable> gradients;
        std::vector<FunctionPtr> functions;
        for (const auto& p : parameters)
        {
            //we do not support sparse gradients for now 
            auto grad = Constant(p.Shape(), p.GetDataType(), 0.0, p.Value()->Device(), L"gradient");
            FunctionPtr result = func(p, grad);
            gradients.push_back(grad);
            functions.push_back(result);
        }
        
        std::vector<Variable> outputs;
        for (auto f : functions)
        {
            for (auto o : f->Outputs())
                outputs.push_back(o);
        }

        ValidateInput(parameters, gradients, Combine(outputs));
    }

    LearnerUniversal::LearnerUniversal(const std::vector<Parameter>& parameters, const std::vector<Variable>& gradients, FunctionPtr updateFunc)
        : LearnerBase(parameters, LearningRateSchedule(Rate(1.0, 1)), AdditionalLearningOptions(), /*allocateSmoothGradients*/ false)
    {
        ValidateInput(parameters, gradients, updateFunc);
    }

    void LearnerUniversal::ValidateInput(const std::vector<Parameter>& parameters, const std::vector<Variable>& gradients, FunctionPtr updateFunc)
    {
        if (parameters.size() != gradients.size())
            LogicError("Number of parameters (%zd) does not match number of gradients (%zd)", parameters.size(), gradients.size());

        if (parameters.size() == 0)
            LogicError("At least 1 parameter is needed in universal learner");

        for (size_t i = 0; i < parameters.size(); ++i)
        {
            auto&& param = parameters[i];
            auto&& grad = gradients[i];
            auto&& inputs = updateFunc->Inputs();
            if (std::find(inputs.begin(), inputs.end(), param) == inputs.end())
                LogicError("Update function does not contain the parameter %ls in its computation", param.AsString().c_str());
            if (std::find(inputs.begin(), inputs.end(), grad) == inputs.end())
                fprintf(stderr, "WARNING: Update function does not contain the gradient for parameter %ls in its computation\n", param.AsString().c_str());
            m_parameter_gradient_map.insert({parameters[i], gradients[i]});
        }
        AllocateDummySmoothedGradients(parameters);
        m_update_func = updateFunc;
    }

    bool LearnerUniversal::Update(std::unordered_map<Parameter, NDArrayViewPtr>& gradientValues, size_t trainingSampleCount, bool sweepEnd)
    {
        ReportTrainingParameterValue(m_learningRateSchedule, L"Learning rate");

        if (LearningRatePerSample(trainingSampleCount) == 0.0)
        {
            return false;
        }

        if (trainingSampleCount == 0)
            InvalidArgument("Learner::Update() cannot perform an update with an empty minibatch.");
        
        static const std::unordered_map<Variable, ValuePtr> m_empty = {};

        for (const auto& parameter : Parameters())
        {
            const auto& gradientValue = gradientValues.at(parameter);
            auto it = m_parameter_gradient_map.find(parameter);
            if (it == m_parameter_gradient_map.end())
                fprintf(stderr, "Parameter %ls does not found in universal learner's list.\n", parameter.AsString().c_str());
            auto grad = Constant(it->second);
            grad.SetValue(gradientValue);
        }

        FunctionPtr update = m_update_func;
        std::unordered_map<Variable, ValuePtr> out;
        for (const auto& o : update->Outputs())
            out.insert({o, nullptr});

        update->Forward(m_empty, out, m_parameters.front().Value()->Device());

        m_sampleCount += trainingSampleCount;
        m_minibatchCount++;
        if (sweepEnd)
        {
            m_sweepCount++;
        }

        return true;
    }

    /*virtual*/ void LearnerUniversal::Update(const Parameter& parameter, const NDArrayViewPtr& gradientValue,
        const NDArrayViewPtr& smoothedGradientValue, size_t trainingSampleCount) const /*override*/
    {
        LogicError("Shouldn't trigger single element update in universal learner.");
    }

    LearnerPtr UniversalLearner(const std::vector<Parameter>& parameters, const ParameterUpdateFunctor& func)
    {
        return MakeSharedObject<LearnerUniversal>(parameters, func);
    }
    
    LearnerPtr UniversalLearner(const std::vector<Parameter>& parameters, const std::vector<Variable>& gradients, FunctionPtr updateFunc)
    {
        return MakeSharedObject<LearnerUniversal>(parameters, gradients, updateFunc);
    }
}
