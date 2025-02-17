//
// Created by qiayuan on 2022/6/24.
//

#include <pinocchio/fwd.hpp>  // forward declarations must be included first.

#include "legged_controllers/LeggedController.h"

#include <ocs2_centroidal_model/AccessHelperFunctions.h>
#include <ocs2_centroidal_model/CentroidalModelPinocchioMapping.h>
#include <ocs2_core/thread_support/ExecuteAndSleep.h>
#include <ocs2_core/thread_support/SetThreadPriority.h>
#include <ocs2_legged_robot_ros/gait/GaitReceiver.h>
#include <ocs2_msgs/mpc_observation.h>
#include <ocs2_pinocchio_interface/PinocchioEndEffectorKinematics.h>
#include <ocs2_ros_interfaces/common/RosMsgConversions.h>
#include <ocs2_ros_interfaces/synchronized_module/RosReferenceManager.h>
#include <ocs2_sqp/SqpMpc.h>
#include "ocs2_robotic_tools/common/RotationTransforms.h"

#include <angles/angles.h>
#include <legged_estimation/FromTopiceEstimate.h>
#include <legged_estimation/LinearKalmanFilter.h>
#include <legged_wbc/HierarchicalWbc.h>
#include <legged_wbc/WeightedWbc.h>
#include <pluginlib/class_list_macros.hpp>



namespace legged {

using namespace convex_plane_decomposition;

// Eigen::MatrixXd traj_optimizedstate;
// Eigen::MatrixXd traj_optimizedinput;
// Eigen::MatrixXd traj_measuredstate;
// Eigen::MatrixXd traj_wbctorque;
Eigen::Vector4d traj_contact;
// Eigen::MatrixXd traj_referencetime;
// Eigen::Matrix<double, 1, 1> traj_referencetime;

// std::string filename = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/optimizedState.csv";
std::string filename_optimizedstate = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/optimizedState.csv";
std::ofstream file_optimizedstate(filename_optimizedstate);

std::string filename_optimizedinput = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/optimizedinput.csv";
std::ofstream file_optimizedinput(filename_optimizedinput);

std::string filename_measuredstate = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/measuredstate.csv";
std::ofstream file_measuredstate(filename_measuredstate);

std::string filename_wbctorque = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/wbctorque.csv";
std::ofstream file_wbctorque(filename_wbctorque);

std::string filename_contactstate = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/contactstate.csv";
std::ofstream file_contactstate(filename_contactstate);

std::string filename_referenceTime = "/home/ustc/robot/ros/ocs2_ws/src/quad_controller/legged_controllers/data/referenceTime.csv";
std::ofstream file_referenceTime(filename_referenceTime);

const static Eigen::IOFormat CSVFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n", "","","\n");

bool LeggedController::init(hardware_interface::RobotHW* robot_hw, ros::NodeHandle& controller_nh) {

  controller_nh.getParam("/if_perceptive", if_perceptive_);

  //! load configuration
  std::string urdfFile;
  std::string taskFile;
  std::string referenceFile;
  controller_nh.getParam("/urdfFile", urdfFile);
  controller_nh.getParam("/taskFile", taskFile);
  controller_nh.getParam("/referenceFile", referenceFile);
  bool verbose = false;
  loadData::loadCppDataType(taskFile, "legged_robot_interface.verbose", verbose);

  //! legged_interface
  setupLeggedInterface(taskFile, urdfFile, referenceFile, verbose);
  setupMpc();
  setupMrt();

  //! Visualization
  ros::NodeHandle nh;
  CentroidalModelPinocchioMapping pinocchioMapping(leggedInterface_->getCentroidalModelInfo());
  eeKinematicsPtr_ = std::make_shared<PinocchioEndEffectorKinematics>(leggedInterface_->getPinocchioInterface(), pinocchioMapping,
                                                                      leggedInterface_->modelSettings().contactNames3DoF);
  robotVisualizer_ = std::make_shared<LeggedRobotVisualizer>(leggedInterface_->getPinocchioInterface(),
                                                             leggedInterface_->getCentroidalModelInfo(), *eeKinematicsPtr_, nh);
  selfCollisionVisualization_.reset(new LeggedSelfCollisionVisualization(leggedInterface_->getPinocchioInterface(),
                                                                         leggedInterface_->getGeometryInterface(), pinocchioMapping, nh));
  //! Hardware interface
  auto* hybridJointInterface = robot_hw->get<HybridJointInterface>();
  std::vector<std::string> joint_names{"LF_HAA", "LF_HFE", "LF_KFE", "LH_HAA", "LH_HFE", "LH_KFE",
                                       "RF_HAA", "RF_HFE", "RF_KFE", "RH_HAA", "RH_HFE", "RH_KFE"};
  for (const auto& joint_name : joint_names) {
    hybridJointHandles_.push_back(hybridJointInterface->getHandle(joint_name));
  }
  auto* contactInterface = robot_hw->get<ContactSensorInterface>();
  for (const auto& name : leggedInterface_->modelSettings().contactNames3DoF) {
    contactHandles_.push_back(contactInterface->getHandle(name));
  }
  imuSensorHandle_ = robot_hw->get<hardware_interface::ImuSensorInterface>()->getHandle("unitree_imu");

  //! State estimation
  setupStateEstimate(taskFile, verbose);

  //! decomposed_elevationMap sub
  if(if_perceptive_)
  {
    auto terrain_callback = [this](const convex_plane_decomposition_msgs::PlanarTerrain::ConstPtr& msg)
    {
      std::unique_ptr<convex_plane_decomposition::PlanarTerrain> newTerrain(
        new convex_plane_decomposition::PlanarTerrain(convex_plane_decomposition::fromMessage(*msg)) );
      planarTerrainPtr.swap(newTerrain);
    };
    terrainSubscriber = controller_nh.subscribe<convex_plane_decomposition_msgs::PlanarTerrain>("/convex_plane_decomposition_ros/planar_terrain", 1, terrain_callback);
  }

  //! Whole body control
  wbc_ = std::make_shared<WeightedWbc>(leggedInterface_->getPinocchioInterface(), leggedInterface_->getCentroidalModelInfo(), *eeKinematicsPtr_);
  wbc_->loadTasksSetting(taskFile, verbose);

  //! Safety Checker
  safetyChecker_ = std::make_shared<SafetyChecker>(leggedInterface_->getCentroidalModelInfo());

  return true;
}

void LeggedController::starting(const ros::Time& time) 
{
  // Initial state
  currentObservation_.state.setZero(leggedInterface_->getCentroidalModelInfo().stateDim);
  updateStateEstimation(time, ros::Duration(0.002));
  currentObservation_.input.setZero(leggedInterface_->getCentroidalModelInfo().inputDim);
  currentObservation_.mode = ModeNumber::STANCE;

  TargetTrajectories target_trajectories({currentObservation_.time}, {currentObservation_.state}, {currentObservation_.input});

  // Set the first observation and command and wait for optimization to finish
  mpcMrtInterface_->setCurrentObservation(currentObservation_);
  mpcMrtInterface_->getReferenceManager().setTargetTrajectories(target_trajectories);
  ROS_INFO_STREAM("Waiting for the initial policy ...");
  while (!mpcMrtInterface_->initialPolicyReceived() && ros::ok()) 
  {
    std::cout << "initialPolicyReceived" << std::endl;
    mpcMrtInterface_->advanceMpc();
    ros::WallRate(leggedInterface_->mpcSettings().mrtDesiredFrequency_).sleep();
  }
  std::cout << "initialPolicyReceived" << std::endl;
  ROS_INFO_STREAM("Initial policy has been received.");

  mpcRunning_ = true;
}


void LeggedController::update(const ros::Time& time, const ros::Duration& period) {
  
  //! subscriber callback
  ros::spinOnce();

  //! State Estimate
  updateStateEstimation(time, period);

  // scalar_t terrain_angle = stateEstimate_->get_terrain_angle();  //! 现在只有cheater_controller中实际用了adaptive_pitch
  scalar_t terrain_angle = 0;
  // std::cout << "terrain_angle: " << terrain_angle << std::endl;

  if(if_perceptive_)
  {
    leggedInterface_->getSwitchedModelReferenceManagerPtr()->update(planarTerrainPtr, terrain_angle); //! update state params for referencemanager
  }

  // std::cout << "currentpose : " << currentObservation_.state.segment<6>(6) << std::endl;

  //! Update the current state of the system
  //! 要改当前的observation的话应该在这改吧
  mpcMrtInterface_->setCurrentObservation(currentObservation_);

  //! Load the latest MPC policy
  mpcMrtInterface_->updatePolicy();

  //! Evaluate the current policy
  vector_t optimizedState, optimizedInput;
  size_t plannedMode = 0;  // The mode that is active at the time the policy is evaluated at.
  mpcMrtInterface_->evaluatePolicy(currentObservation_.time, currentObservation_.state, optimizedState, optimizedInput, plannedMode);

  currentObservation_.input = optimizedInput;

  // std::cout << "optimizedpose: " << optimizedState.segment<6>(6) << std::endl;
  // std::cout << "delta_pose: " << optimizedState.segment<6>(6) - currentObservation_.state.segment<6>(6);
  // std::cout << "centrodial_vel: " << optimizedState.segment<3>(0) << std::endl;
  // std::cout << "centroidal angular velocity: " << optimizedState.segment<3>(3) << std::endl; 
  // std::cout << "optimizedState: " << optimizedState << std::endl; //! STATE: angular velocity, linear velocity, x, y, z, yaw, pitch, roll, 12 joint angles
  // std::cout << "optimizedInput: " << optimizedInput << std::endl; //! INPUT: LF_F, RF_F, LH_F, RH_F, 12 joint velocities
  // std::cout << "measuredRbdState_: " << measuredRbdState_ << std::endl; //! measuredRbdState_:  yaw, pitch, roll, x, y, z, 12 joint angles, angular velocity, linear velocity, 12 joint velocities

  //! WBC接受的是高度是机器人自己的质心高度

  //! Whole body control
  //! the optimized motion planned by the mpc layers consists of contact forces and desired joint velocities
  wbcTimer_.startTimer();
  vector_t x = wbc_->update(optimizedState, optimizedInput, measuredRbdState_, plannedMode, period.toSec());
  wbcTimer_.endTimer();

  vector_t torque = x.tail(12);
  // std::cout << "wbc x:" << x << std::endl;  // 

  vector_t posDes = centroidal_model::getJointAngles(optimizedState, leggedInterface_->getCentroidalModelInfo()); // optimized state
  vector_t velDes = centroidal_model::getJointVelocities(optimizedInput, leggedInterface_->getCentroidalModelInfo()); // optimized velocity

  // traj_referencetime << currentObservation_.time;
  // std::cout << "here" << std::endl;

  //!------------------------------------------------record here----------------------------------------------!//
  file_optimizedstate << optimizedState.transpose().format(CSVFormat);
  file_optimizedinput << optimizedInput.transpose().format(CSVFormat);
  file_measuredstate << measuredRbdState_.transpose().format(CSVFormat);
  file_wbctorque << torque.transpose().format(CSVFormat);
  file_contactstate << traj_contact.transpose().format(CSVFormat);
  file_referenceTime << currentObservation_.time << std::endl;

  //!------------------------------------------------record here----------------------------------------------!//

  //! Safety check, if failed, stop the controller
  if (!safetyChecker_->check(currentObservation_, optimizedState, optimizedInput)) 
  {
    ROS_ERROR_STREAM("[Legged Controller] Safety check failed, stopping the controller.");
    stopRequest(time);
  }

  //! set cmd
  for (size_t j = 0; j < leggedInterface_->getCentroidalModelInfo().actuatedDofNum; ++j) 
  {
    hybridJointHandles_[j].setCommand(posDes(j), velDes(j), 5, 3, torque(j));   // default: kp=0 kd=3
  }

  //! Visualization
  robotVisualizer_->update(currentObservation_, mpcMrtInterface_->getPolicy(), mpcMrtInterface_->getCommand()); // (const SystemObservation& observation, const PrimalSolution& primalSolution, const CommandData& command) 
  selfCollisionVisualization_->update(currentObservation_);

  //! Publish the observation. Only needed for the command interface
  observationPublisher_.publish(ros_msg_conversions::createObservationMsg(currentObservation_));
}

void LeggedController::updateReferenceManager()
{
  // vector3_t velReal_baseframe = stateEstimate_->getvel_basefrane();   // x y z三轴方向上的速度

  // vector3_t Base2World_Trans = measuredRbdState_.segment<3>(3);

  // vector3_t Base2World_ZYX = measuredRbdState_.segment<3>(0);
  // Eigen::Matrix<double, 3, 3> Base2World_RotationMatrix = getRotationMatrixFromZyxEulerAngles(Base2World_ZYX);

  // vector_t foot_height = stateEstimate_->get_localfoot_H();

  // leggedInterface_->getSwitchedModelReferenceManagerPtr()->update(planarTerrainPtr);

}

void LeggedController::updateStateEstimation(const ros::Time& time, const ros::Duration& period) 
{
  vector_t jointPos(hybridJointHandles_.size()), jointVel(hybridJointHandles_.size());
  contact_flag_t contacts;
  Eigen::Quaternion<scalar_t> quat;
  contact_flag_t contactFlag;
  vector3_t angularVel, linearAccel;
  matrix3_t orientationCovariance, angularVelCovariance, linearAccelCovariance;
  orientationCovariance << 0.0012, 0, 0,
                           0, 0.0012, 0,
                           0, 0, 0.0012;
  angularVelCovariance << 0.0004, 0, 0,
                          0, 0.0004, 0,
                          0, 0, 0.0004;
  linearAccelCovariance << 0.01, 0, 0,
                           0, 0.01, 0,
                           0, 0, 0.01;                       

  for (size_t i = 0; i < hybridJointHandles_.size(); ++i) 
  {
    jointPos(i) = hybridJointHandles_[i].getPosition();
    jointVel(i) = hybridJointHandles_[i].getVelocity();
  }
  for (size_t i = 0; i < contacts.size(); ++i) {
    contactFlag[i] = contactHandles_[i].isContact();
    // std::cout << "contactFlag " << i << " : " << contactFlag[i] << std::endl;
    traj_contact(i) = contactHandles_[i].isContact();
  }
  // std::cout << "traj_contact: " << traj_contact << std::endl; 
  for (size_t i = 0; i < 4; ++i) {
    quat.coeffs()(i) = imuSensorHandle_.getOrientation()[i];
  }
  for (size_t i = 0; i < 3; ++i) {
    angularVel(i) = imuSensorHandle_.getAngularVelocity()[i];
    linearAccel(i) = imuSensorHandle_.getLinearAcceleration()[i];
  }
  // for (size_t i = 0; i < 9; ++i) {
  //   orientationCovariance(i) = imuSensorHandle_.getOrientationCovariance()[i];
  //   angularVelCovariance(i) = imuSensorHandle_.getAngularVelocityCovariance()[i];
  //   linearAccelCovariance(i) = imuSensorHandle_.getLinearAccelerationCovariance()[i];
  // }

  //! Publish imu, jointState for Cerberus

  //! update rbdState_Joint
  stateEstimate_->updateJointStates(jointPos, jointVel);  // 更新关节位置速度
  //! update contactFlag_
  stateEstimate_->updateContact(contactFlag); // 更新足端接触状态
  //! update rbdState_Angular
  stateEstimate_->updateImu(quat, angularVel, linearAccel, orientationCovariance, angularVelCovariance, linearAccelCovariance); // 更新IMU读数
  //! use kalman filter to update rbdState_linear
  measuredRbdState_ = stateEstimate_->update(time, period); // 更新自定义的 measuredRbdState_
  //// use Cerberus update rbdState_linearSP,linearPos,AngularPos
  

  //! update ocs2 observation_state
  currentObservation_.time += period.toSec();
  scalar_t yawLast = currentObservation_.state(9);
  currentObservation_.state = rbdConversions_->computeCentroidalStateFromRbdModel(measuredRbdState_); // 更新ocs2中的currentObservation_
  //! 是不是这里的state 不太对,导致上楼有问题
  currentObservation_.state(9) = yawLast + angles::shortest_angular_distance(yawLast, currentObservation_.state(9));
  currentObservation_.mode = stateEstimate_->getMode();
}

LeggedController::~LeggedController() {
  controllerRunning_ = false;
  if (mpcThread_.joinable()) {
    mpcThread_.join();
  }

  file_optimizedstate.close();
  file_optimizedinput.close();
  file_measuredstate.close();
  file_wbctorque.close();
  file_referenceTime.close();
  std::cerr << "\n traj file closed" << std::endl;

  std::cerr << "########################################################################";
  std::cerr << "\n### MPC Benchmarking";
  std::cerr << "\n###   Maximum : " << mpcTimer_.getMaxIntervalInMilliseconds() << "[ms].";
  std::cerr << "\n###   Average : " << mpcTimer_.getAverageInMilliseconds() << "[ms]." << std::endl;
  std::cerr << "########################################################################";
  std::cerr << "\n### WBC Benchmarking";
  std::cerr << "\n###   Maximum : " << wbcTimer_.getMaxIntervalInMilliseconds() << "[ms].";
  std::cerr << "\n###   Average : " << wbcTimer_.getAverageInMilliseconds() << "[ms].";

}

void LeggedController::setupLeggedInterface(const std::string& taskFile, const std::string& urdfFile, const std::string& referenceFile,
                                            bool verbose) {
  leggedInterface_ = std::make_shared<LeggedInterface>(taskFile, urdfFile, referenceFile);
  leggedInterface_->setupOptimalControlProblem(taskFile, urdfFile, referenceFile, verbose);
}

void LeggedController::setupMpc() {
  mpc_ = std::make_shared<SqpMpc>(leggedInterface_->mpcSettings(), leggedInterface_->sqpSettings(),
                                  leggedInterface_->getOptimalControlProblem(), leggedInterface_->getInitializer());
  rbdConversions_ = std::make_shared<CentroidalModelRbdConversions>(leggedInterface_->getPinocchioInterface(),
                                                                    leggedInterface_->getCentroidalModelInfo());

  const std::string robotName = "legged_robot";
  ros::NodeHandle nh;

  // Gait receiver
  auto gaitReceiverPtr =
      std::make_shared<GaitReceiver>(nh, leggedInterface_->getSwitchedModelReferenceManagerPtr()->getGaitSchedule(), robotName);

  // ROS ReferenceManager
  auto rosReferenceManagerPtr = std::make_shared<RosReferenceManager>(robotName, leggedInterface_->getReferenceManagerPtr());
  rosReferenceManagerPtr->subscribe(nh);
  mpc_->getSolverPtr()->addSynchronizedModule(gaitReceiverPtr);
  mpc_->getSolverPtr()->setReferenceManager(rosReferenceManagerPtr);
  observationPublisher_ = nh.advertise<ocs2_msgs::mpc_observation>(robotName + "_mpc_observation", 1);
}

void LeggedController::setupMrt() {
  mpcMrtInterface_ = std::make_shared<MPC_MRT_Interface>(*mpc_);
  mpcMrtInterface_->initRollout(&leggedInterface_->getRollout());
  mpcTimer_.reset();

  controllerRunning_ = true;
  mpcThread_ = std::thread([&]() {
    while (controllerRunning_) {
      try {
        executeAndSleep(
            [&]() {
              if (mpcRunning_) {
                mpcTimer_.startTimer();
                mpcMrtInterface_->advanceMpc();
                mpcTimer_.endTimer();
              }
            },
            leggedInterface_->mpcSettings().mpcDesiredFrequency_);
      } catch (const std::exception& e) {
        controllerRunning_ = false;
        ROS_ERROR_STREAM("[Ocs2 MPC thread] Error : " << e.what());
        stopRequest(ros::Time());
      }
    }
  });
  setThreadPriority(leggedInterface_->sqpSettings().threadPriority, mpcThread_);
}

void LeggedController::setupStateEstimate(const std::string& taskFile, bool verbose) 
{
  stateEstimate_ = std::make_shared<KalmanFilterEstimate>(leggedInterface_->getPinocchioInterface(),
                                                          leggedInterface_->getCentroidalModelInfo(), *eeKinematicsPtr_);
  dynamic_cast<KalmanFilterEstimate&>(*stateEstimate_).loadSettings(taskFile, verbose);
  currentObservation_.time = 0;
}

void LeggedCheaterController::setupStateEstimate(const std::string& /*taskFile*/, bool /*verbose*/) 
{
  stateEstimate_ = std::make_shared<FromTopicStateEstimate>(leggedInterface_->getPinocchioInterface(),
                                                            leggedInterface_->getCentroidalModelInfo(), *eeKinematicsPtr_);
}

}  // namespace legged

PLUGINLIB_EXPORT_CLASS(legged::LeggedController, controller_interface::ControllerBase)
PLUGINLIB_EXPORT_CLASS(legged::LeggedCheaterController, controller_interface::ControllerBase)
