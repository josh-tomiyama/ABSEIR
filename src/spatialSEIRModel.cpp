#include <Rcpp.h>
#include <Eigen/Core>
#include <RcppEigen.h>
#include <cmath>
#include <math.h>
#include <spatialSEIRModel.hpp>
#include <dataModel.hpp>
#include <exposureModel.hpp>
#include <reinfectionModel.hpp>
#include <distanceModel.hpp>
#include <transitionPriors.hpp>
#include <initialValueContainer.hpp>
#include <samplingControl.hpp>
#include <util.hpp>
#include <SEIRSimNodes.hpp>

using namespace Rcpp;

std::vector<size_t> sort_indexes(Rcpp::NumericVector inVec)
{
    vector<size_t> idx(inVec.size());
    for (size_t i = 0; i < idx.size(); i++)
    {
        idx[i] = i;
    }
    std::sort(idx.begin(), idx.end(),
         [&inVec](size_t i1, size_t i2){return(inVec(i1) < inVec(i2));});
    return(idx);    
}

spatialSEIRModel::spatialSEIRModel(dataModel& dataModel_,
                                   exposureModel& exposureModel_,
                                   reinfectionModel& reinfectionModel_,
                                   distanceModel& distanceModel_,
                                   transitionPriors& transitionPriors_,
                                   initialValueContainer& initialValueContainer_,
                                   samplingControl& samplingControl_)
{
    // Make sure these pointers go to the real deal
    int err = (((dataModel_.getModelComponentType()) != LSS_DATA_MODEL_TYPE) ||
            ((exposureModel_.getModelComponentType()) != LSS_EXPOSURE_MODEL_TYPE) ||
            ((reinfectionModel_.getModelComponentType()) != LSS_REINFECTION_MODEL_TYPE) ||
            ((distanceModel_.getModelComponentType()) != LSS_DISTANCE_MODEL_TYPE) ||
            ((transitionPriors_.getModelComponentType()) != LSS_TRANSITION_MODEL_TYPE) ||
            ((initialValueContainer_.getModelComponentType()) != LSS_INIT_CONTAINER_TYPE) ||
            ((samplingControl_.getModelComponentType()) != LSS_SAMPLING_CONTROL_MODEL_TYPE));
    if (err != 0)
    { 
        Rcpp::stop("Error: model components were not provided in the correct order. \n");
    }

    // Store model components
    dataModelInstance = &dataModel_;
    exposureModelInstance = &exposureModel_;
    reinfectionModelInstance = &reinfectionModel_;
    distanceModelInstance = &distanceModel_;
    transitionPriorsInstance = &transitionPriors_;
    initialValueContainerInstance = &initialValueContainer_;
    samplingControlInstance = &samplingControl_;

    // Check for model component compatibility
    if ((dataModelInstance -> nLoc) != (exposureModelInstance -> nLoc))
    { 
        Rcpp::stop(("Exposure model and data model imply different number of locations: " 
                + std::to_string(dataModelInstance -> nLoc) + ", " 
                + std::to_string(exposureModelInstance -> nLoc) + ".\n").c_str());
    }
    if ((dataModelInstance -> nTpt) != (exposureModelInstance -> nTpt))
    { 
        Rcpp::stop(("Exposure model and data model imply different number of time points:"
                    + std::to_string(dataModelInstance -> nTpt) + ", "
                    + std::to_string(exposureModelInstance -> nTpt) + ".\n").c_str());  
    }
    if ((dataModelInstance -> nLoc) != (distanceModelInstance -> numLocations))
    {       
        Rcpp::stop(("Data model and distance model imply different number of locations:"
                    + std::to_string(dataModelInstance -> nLoc) + ", "
                    + std::to_string(distanceModelInstance -> numLocations) + ".\n").c_str()
                );
    }
    if ((distanceModelInstance -> tdm_list).size() != (dataModelInstance -> nTpt))
    {
        Rcpp::stop("TDistance model and data model imply a different number of time points.\n");
    }
    int sz1 = (distanceModelInstance -> tdm_list)[0].size();
    for (int i = 0; i < (distanceModelInstance -> tdm_list).size(); i++)
    {
        if (distanceModelInstance -> tdm_list[i].size() != sz1)
        {
            Rcpp::stop("Differing number of lagged contact matrices across time points.\n");
        }
    }
    if ((dataModelInstance -> nLoc) != (initialValueContainerInstance -> S0.size())) 
    { 
        Rcpp::stop("Data model and initial value container have different dimensions\n");
    }
    if ((reinfectionModelInstance -> reinfectionMode) == 3)
    {
        // No reinfection
    }
    else
    {
        if (((reinfectionModelInstance -> X_rs).rows()) != (dataModelInstance -> nTpt))
        { 
            Rcpp::stop("Reinfection and data mode time points differ.\n");
        }
    }
    if ((reinfectionModelInstance -> reinfectionMode) > 2)
    {
        // pass
    }

    if (transitionPriorsInstance -> mode != "exponential" && 
            transitionPriorsInstance -> mode != "path_specific" &&
            transitionPriorsInstance -> mode != "weibull")
    {
        Rcpp::stop("Invalid transition mode: " + 
                (transitionPriorsInstance -> mode));
    }

    // Optionally, set up transition distribution
    if (transitionPriorsInstance -> mode == "weibull")
    {
        EI_transition_dist = std::unique_ptr<weibullTransitionDistribution>(
                new weibullTransitionDistribution(
                (transitionPriorsInstance -> E_to_I_params).col(0))); 
        IR_transition_dist = std::unique_ptr<weibullTransitionDistribution>(
                new weibullTransitionDistribution(
                (transitionPriorsInstance -> I_to_R_params).col(0))); 
    }
    else
    {
        Eigen::VectorXd DummyParams(4);
        DummyParams(0) = 1.0;
        DummyParams(1) = 1.0;
        DummyParams(2) = 1.0;
        DummyParams(3) = 1.0;
        EI_transition_dist = std::unique_ptr<weibullTransitionDistribution>(new 
            weibullTransitionDistribution(DummyParams));
        IR_transition_dist = std::unique_ptr<weibullTransitionDistribution>(new 
            weibullTransitionDistribution(DummyParams));
    }

    // Set up random number provider 
    std::minstd_rand0 lc_generator(samplingControlInstance -> random_seed + 1);
    std::uint_least32_t seed_data[std::mt19937::state_size];
    std::generate_n(seed_data, std::mt19937::state_size, std::ref(lc_generator));
    std::seed_seq q(std::begin(seed_data), std::end(seed_data));
    generator = new std::mt19937{q};   

    // Parameters are not initialized
    is_initialized = false;

    // Set up places for worker nodes to put stuff
    results_complete = std::vector<simulationResultSet>();
    results_double = Eigen::MatrixXd::Zero(samplingControlInstance -> batch_size, 
                                           samplingControlInstance -> m); 

    // Create the worker pool
    worker_pool = std::unique_ptr<NodePool>(
                new NodePool(&results_double,
                     &results_complete,
                     &result_idx,
                     (unsigned int) samplingControlInstance -> CPU_cores,
                     samplingControlInstance->random_seed + ncalls,
                     initialValueContainerInstance -> S0,
                     initialValueContainerInstance -> E0,
                     initialValueContainerInstance -> I0,
                     initialValueContainerInstance -> R0,
                     exposureModelInstance -> offset,
                     dataModelInstance -> Y,
                     dataModelInstance -> na_mask,
                     distanceModelInstance -> dm_list,
                     distanceModelInstance -> tdm_list,
                     distanceModelInstance -> tdm_empty,
                     exposureModelInstance -> X,
                     reinfectionModelInstance -> X_rs,
                     transitionPriorsInstance -> mode,
                     transitionPriorsInstance -> E_to_I_params,
                     transitionPriorsInstance -> I_to_R_params,
                     transitionPriorsInstance -> inf_mean,
                     distanceModelInstance -> spatial_prior,
                     exposureModelInstance -> betaPriorPrecision,
                     reinfectionModelInstance -> betaPriorPrecision, 
                     exposureModelInstance -> betaPriorMean,
                     reinfectionModelInstance -> betaPriorMean,
                     dataModelInstance -> phi,
                     dataModelInstance -> dataModelCompartment,
                     dataModelInstance -> cumulative,
                     samplingControlInstance -> m
                ));
}

Eigen::MatrixXd spatialSEIRModel::generateParamsPrior(int nParticles)
{
    const bool hasReinfection = (reinfectionModelInstance -> 
            betaPriorPrecision)(0) > 0;
    const bool hasSpatial = (dataModelInstance -> Y).cols() > 1;
    std::string transitionMode = transitionPriorsInstance -> mode;

    const int nBeta = (exposureModelInstance -> X).cols();
    const int nBetaRS = (reinfectionModelInstance -> X_rs).cols()*hasReinfection;
    const int nRho = ((distanceModelInstance -> dm_list).size() + 
                      (distanceModelInstance -> tdm_list)[0].size())*hasSpatial;
    const int nTrans = (transitionMode == "exponential" ? 2 :
                       (transitionMode == "weibull" ? 4 : 0));

    const int nParams = nBeta + nBetaRS + nRho + nTrans;

    int i, j;

    Eigen::MatrixXd outParams = Eigen::MatrixXd(N, nParams)

    // Set up random samplers 
    // beta, beta_RS
    std::normal_distribution<> standardNormal(0,1); 
    // rho  
    std::gamma_distribution<> rhoDist(
            (distanceModelInstance -> spatial_prior)(0),
        1.0/(distanceModelInstance -> spatial_prior)(1));
    // Hyperprior distributions for E to I and I to R transitions
    std::vector<std::gamma_distribution<> > gammaEIDist;
    std::vector<std::gamma_distribution<> > gammaIRDist;

    if (transitionMode == "exponential")
    {
        gammaEIDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> E_to_I_params)(0,0),
            1.0/(transitionPriorsInstance -> E_to_I_params)(1,0)));
        gammaIRDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> I_to_R_params)(0,0),
            1.0/(transitionPriorsInstance -> I_to_R_params)(1,0)));
    }   
    else if (transitionMode == "weibull")
    {
        gammaEIDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> E_to_I_params)(0,0),
            1.0/(transitionPriorsInstance -> E_to_I_params)(1,0)));
        gammaEIDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> E_to_I_params)(2,0),
            1.0/(transitionPriorsInstance -> E_to_I_params)(3,0)));

        gammaIRDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> I_to_R_params)(0,0),
            1.0/(transitionPriorsInstance -> I_to_R_params)(1,0)));
        gammaIRDist.push_back(std::gamma_distribution<>(
                (transitionPriorsInstance -> I_to_R_params)(2,0),
            1.0/(transitionPriorsInstance -> I_to_R_params)(3,0)));
    }
    // If this is too slow, consider column-wise operations
    double rhoTot = 0.0;
    int rhoItrs = 0;
    for (i = 0; i < nParticles; i++)
    {
        // Draw beta
        for (j = 0; j < nBeta; j++)
        {
            outParams(i, j) = (exposureModelInstance -> betaPriorMean(j)) + 
                                 standardNormal(*generator) /
                                 (exposureModelInstance -> betaPriorPrecision(j));
        }
    }
    // draw gammaEI, gammaIR
    if (transitionMode == "exponential")
    {
        for (i = 0; i < nParticles; i++)
        {
            // Draw gamma_ei
            outParams(i, nBeta + nBetaRS + nRho) = 
                gammaEIDist[0](*generator);
            // Draw gamma_ir
            outParams(i, nBeta + nBetaRS + nRho + 1) = 
                gammaIRDist[0](*generator);
        }
    }
    else if (transitionMode == "weibull")
    {
        for (i = 0; i < nParticles; i++)
        {
            outParams(i, nBeta + nBetaRS + nRho) = gammaEIDist[0](*generator);
            outParams(i, nBeta + nBetaRS + nRho + 1) = gammaEIDist[1](*generator);
            outParams(i, nBeta + nBetaRS + nRho + 2) = gammaIRDist[0](*generator);
            outParams(i, nBeta + nBetaRS + nRho + 3) = gammaIRDist[1](*generator);
        }
    }
    // Draw reinfection parameters
    if (hasReinfection)
    {
        for (i = 0; i < nParticles; i++)
        {
            for (j = nBeta; j < nBeta + nBetaRS; j++)
            {
                outParams(i, j) = 
                    (reinfectionModelInstance -> betaPriorMean(j)) + 
                     standardNormal(*generator) /
                    (reinfectionModelInstance -> betaPriorPrecision(j));           
            }
        }
    }
    // Draw rho
    if (hasSpatial)
    {
        for (i = 0; i < nParticles; i++)
        {
            rhoTot = 2.0; 
            rhoItrs = 0;
            while (rhoTot > 1.0 && rhoItrs < 100)
            {
                rhoTot = 0.0;
                for (j = nBeta + nBetaRS; j < nBeta + nBetaRS + nRho; j++)
                {
                   outParams(i, j) = rhoDist(*generator); 
                   rhoTot += outParams(i,j);
                }
                rhoItrs++;
            }
            if (rhoTot > 1.0)
            {
                Rcpp::Rcout << "Error, valid rho value not obtained\n";
            }
        }
    }
}

bool spatialSEIRModel::setParameters(Eigen::MatrixXd params)
{
    const int nBeta = (exposureModelInstance -> X).cols();
    const int nBetaRS = (reinfectionModelInstance -> X_rs).cols()*hasReinfection;
    const int nRho = ((distanceModelInstance -> dm_list).size() + 
                      (distanceModelInstance -> tdm_list)[0].size())*hasSpatial;

    const int nTrans = (transitionMode == "exponential" ? 2 :
                       (transitionMode == "weibull" ? 4 : 0));
    const int nParams = nBeta + nBetaRS + nRho + nTrans;
    
    if (params.cols() != nParams)
    {
        Rcpp::stop("Number of supplied parameters does not match model specification.\n");
    }

    param_matrix = params; 
    is_initialized = true;
}

double spatialSEIRModel::evalPrior(Eigen::VectorXd param_vector)
{
    double outPrior = 1.0;
    double constr = 0.0;
    const bool hasReinfection = (reinfectionModelInstance -> betaPriorPrecision)(0) > 0;
    const bool hasSpatial = (dataModelInstance -> Y).cols() > 1;
    std::string transitionMode = transitionPriorsInstance -> mode;
    const int nBeta = (exposureModelInstance -> X).cols();
    const int nBetaRS = (reinfectionModelInstance -> X_rs).cols()*hasReinfection;
    const int nRho = ((distanceModelInstance -> dm_list).size() + 
                      (distanceModelInstance -> tdm_list)[0].size())*hasSpatial;

    int i;

    int paramIdx = 0;
    for (i = 0; i < nBeta; i++)
    {
        outPrior *= R::dnorm(param_vector(paramIdx), 
                (exposureModelInstance -> betaPriorMean)(i), 
                1.0/((exposureModelInstance -> betaPriorPrecision)(i)), 0);
        paramIdx++;
    }

    if (nBetaRS > 0)
    {
        for (i = 0; i < nBetaRS; i++)
        {
            outPrior *= R::dnorm(param_vector(paramIdx), 
                    (reinfectionModelInstance -> betaPriorMean)(i), 
                    1.0/((reinfectionModelInstance -> betaPriorPrecision)(i)), 0);
            paramIdx++;
        }
    }

    if (nRho > 0)
    {
        for (i = 0; i < nRho; i++)
        {
             constr += param_vector(paramIdx);
             outPrior *= R::dbeta(param_vector(paramIdx), 
                         (distanceModelInstance -> spatial_prior)(0),
                         (distanceModelInstance -> spatial_prior)(1), 0);
             paramIdx++;
        }
        outPrior *= (constr <= 1);
    }

    if (transitionMode == "exponential")
    {
        outPrior *= R::dgamma(param_vector(paramIdx), 
                (transitionPriorsInstance -> E_to_I_params)(0,0),
                1.0/(transitionPriorsInstance -> E_to_I_params)(1,0), 0);
        paramIdx++;

        outPrior *= R::dgamma(param_vector(paramIdx), 
                (transitionPriorsInstance -> I_to_R_params)(0,0),
                1.0/(transitionPriorsInstance -> I_to_R_params)(1,0), 0);
    }
    else if (transitionMode == "weibull")
    {
        outPrior *= EI_transition_dist -> evalParamPrior(
                param_vector.segment(paramIdx, 2)); 
        paramIdx += 2;
        outPrior *= IR_transition_dist -> evalParamPrior(
                param_vector.segment(paramIdx, 2)); 
        paramIdx += 2;
    }
    return(outPrior);
}

void spatialSEIRModel::run_simulation(Eigen::MatrixXd params)
{
    // To-do: run simulation logic here
}

spatialSEIRModel::~spatialSEIRModel()
{   
    delete generator;
}

RCPP_MODULE(mod_spatialSEIRModel)
{
    using namespace Rcpp;
    class_<spatialSEIRModel>( "spatialSEIRModel" )
    .constructor<dataModel&,
                 exposureModel&,
                 reinfectionModel&,
                 distanceModel&,
                 transitionPriors&,
                 initialValueContainer&,
                 samplingControl&>()
    .method("sample", &spatialSEIRModel::sample)
    .method("setParameters", &spatialSEIRModel::setParameters)
}

