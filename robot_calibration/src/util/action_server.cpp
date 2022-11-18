#include <robot_calibration/util/action_server.hpp>
#include <robot_calibration/util/poses_from_yaml.hpp>

#include <robot_calibration/optimization/ceres_optimizer.hpp>
#include <robot_calibration/optimization/export.hpp>

#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Matrix3x3.h"
#include "tf2/LinearMath/Transform.h"

using namespace std::placeholders;

CalibratePoseServer::CalibratePoseServer(const rclcpp::Node::SharedPtr& node, std::string robot_poses_path)
: node_(node), pub_{node_->create_publisher<visualization_msgs::msg::MarkerArray>("/cal", rclcpp::SensorDataQoS().reliable())}, robot_poses_path_(robot_poses_path)
{
  action_server_ = rclcpp_action::create_server<CalibratePose>(
      node_,
      "calibrate_pose",
      std::bind(&CalibratePoseServer::handle_goal, this, _1, _2),
      std::bind(&CalibratePoseServer::handle_cancel, this, _1),
      std::bind(&CalibratePoseServer::handle_accepted, this, _1));

  tf_buffer_ =
    std::make_unique<tf2_ros::Buffer>(node->get_clock());
  tf_listener_ =
    std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

  load_robot_poses();

  capture_manager.init(node_);

  // Load calibration steps
  calibration_steps =
      node_->declare_parameter<std::vector<std::string>>("calibration_steps", std::vector<std::string>());
  if (calibration_steps.empty())
  {
    RCLCPP_FATAL(node->get_logger(), "Parameter calibration_steps is not defined");
  }
}

void CalibratePoseServer::load_robot_poses()
{
  RCLCPP_INFO(node_->get_logger(), "Loading YAML calibration poses from %s", robot_poses_path_.c_str());
    if (!robot_calibration::getPosesFromYaml(robot_poses_path_, robot_poses))
    {
      // Error will be printed in function
      RCLCPP_FATAL(node_->get_logger(), "Failed get get calibration poses from YAML file");
    }
}

rclcpp_action::GoalResponse CalibratePoseServer::handle_goal(
  const rclcpp_action::GoalUUID & uuid,
  std::shared_ptr<const CalibratePose::Goal> goal)
{
  RCLCPP_INFO(node_->get_logger(), "Received goal request");
  (void)uuid;
  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse CalibratePoseServer::handle_cancel(
  const std::shared_ptr<GoalHandleCalibratePose> goal_handle)
{
  RCLCPP_INFO(node_->get_logger(), "Received request to cancel goal");
  (void)goal_handle;
  return rclcpp_action::CancelResponse::ACCEPT;
}

void CalibratePoseServer::handle_accepted(const std::shared_ptr<GoalHandleCalibratePose> goal_handle)
{
  std::thread{std::bind(&CalibratePoseServer::execute, this, _1), goal_handle}.detach();
}

void CalibratePoseServer::execute(const std::shared_ptr<GoalHandleCalibratePose> goal_handle) {
  auto logger = node_->get_logger();
  const auto goal = goal_handle->get_goal();

  // Save URDF for calibration/export step
  description_msg.data = capture_manager.getUrdf();

  // Loop through robot poses if read from YAML or bag file
  if (!robot_poses.empty()) 
  {
    for (unsigned pose_idx = 0; pose_idx < robot_poses.size() && rclcpp::ok();
        ++pose_idx)
    {
      robot_calibration_msgs::msg::CalibrationData msg;

      // Move head/arm to pose
      if (!capture_manager.moveToState(robot_poses[pose_idx].joint_states))
      {
        RCLCPP_WARN(logger, "Unable to move to desired state for sample %u.", pose_idx);
        continue;
      }

      // Make sure sensor data is up to date after settling
      rclcpp::sleep_for(std::chrono::milliseconds(2000));

      // Get pose of the features
      if (!capture_manager.captureFeatures(robot_poses[pose_idx].features, msg))
      {
        RCLCPP_WARN(logger, "Failed to capture sample %u.", pose_idx);
        continue;
      }

      RCLCPP_INFO(logger, "Captured pose %u of %lu", pose_idx + 1, robot_poses.size());

      // Add to samples
      std::cout << "Number of observation : " << msg.observations.size() << std::endl;
      std::cout << "Name of observation sensor 1 : " << msg.observations[0].sensor_name << std::endl;
      std::cout << "Name of observation sensor 2 : " << msg.observations[1].sensor_name << std::endl;
      data.push_back(msg);
    }

    RCLCPP_INFO(logger, "Done capturing samples");
  }

  // Create instance of optimizer
  robot_calibration::OptimizationParams params;
  robot_calibration::Optimizer opt(description_msg.data);

  // Run calibration steps
  for (auto step : calibration_steps)
  {
    params.LoadFromROS(node_, step);

    // If we receive initial guess from action goal, override 
    if (goal->estimated_poses.size() > 0) {
      for (unsigned i=0; i < params.free_frames.size(); i++) {
        for (const auto& estimated_pose : goal->estimated_poses) {
          if (estimated_pose.child_frame_id == params.free_frames.at(i).name) {
            std::cout << "Overriding estimated poses from action goal for frame : " << params.free_frames.at(i).name << std::endl;
            std::cout << "Initial Value - x: " <<  params.free_frames_initial_values.at(i).x << " y: " <<  params.free_frames_initial_values.at(i).y << " z: " <<  params.free_frames_initial_values.at(i).z << " roll: " <<  params.free_frames_initial_values.at(i).roll << " pitch: " <<  params.free_frames_initial_values.at(i).pitch << " yaw: " <<  params.free_frames_initial_values.at(i).yaw << std::endl;
            params.free_frames_initial_values.at(i).x = estimated_pose.transform.translation.x;
            params.free_frames_initial_values.at(i).y = estimated_pose.transform.translation.y;
            params.free_frames_initial_values.at(i).z = estimated_pose.transform.translation.z;

            tf2::Quaternion q(estimated_pose.transform.rotation.x,
                              estimated_pose.transform.rotation.y,
                              estimated_pose.transform.rotation.z,
                              estimated_pose.transform.rotation.w);
            tf2::Matrix3x3 m(q);
            double roll, pitch, yaw;
            m.getRPY(roll, pitch, yaw);

            params.free_frames_initial_values[i].roll = roll;
            params.free_frames_initial_values[i].pitch = pitch;
            params.free_frames_initial_values[i].yaw = yaw;

            std::cout << "Overriden Value - x: " <<  estimated_pose.transform.translation.x << " y: " << estimated_pose.transform.translation.y << " z: " <<  estimated_pose.transform.translation.z << " roll: " << roll << " pitch: " <<  pitch << " yaw: " <<  yaw << std::endl;

          }
        }
      }
    }

    geometry_msgs::msg::TransformStamped initial_estimate;
    if (goal->lookup_initial_estimate) {
      try {
        RCLCPP_INFO(logger, "Looking up transform from base_link to scene_camera_mount_link");
        initial_estimate = tf_buffer_->lookupTransform("base_link", "scene_camera_mount_link", tf2::TimePointZero);
      } catch (const tf2::TransformException & ex) {
        RCLCPP_INFO(logger, "Could not transform scene_camera_mount_link to base_link : %s", ex.what());
      }

      for (unsigned i=0; i < params.free_frames.size(); i++) {
        if (params.free_frames.at(i).name == "scene_camera_mount_joint") {
          std::cout << "Looking up transform for scene_camera_mound_joint" << std::endl;
          std::cout << "Initial Value - x: " <<  params.free_frames_initial_values.at(i).x << " y: " <<  params.free_frames_initial_values.at(i).y << " z: " <<  params.free_frames_initial_values.at(i).z << " roll: " <<  params.free_frames_initial_values.at(i).roll << " pitch: " <<  params.free_frames_initial_values.at(i).pitch << " yaw: " <<  params.free_frames_initial_values.at(i).yaw << std::endl;

          params.free_frames_initial_values.at(i).x = initial_estimate.transform.translation.x;
          params.free_frames_initial_values.at(i).y = initial_estimate.transform.translation.y;
          params.free_frames_initial_values.at(i).z = initial_estimate.transform.translation.z;

          tf2::Quaternion q(initial_estimate.transform.rotation.x,
                            initial_estimate.transform.rotation.y,
                            initial_estimate.transform.rotation.z,
                            initial_estimate.transform.rotation.w);
          tf2::Matrix3x3 m(q);
          double roll, pitch, yaw;
          m.getRPY(roll, pitch, yaw);

          params.free_frames_initial_values.at(i).roll = roll;
          params.free_frames_initial_values.at(i).pitch = pitch;
          params.free_frames_initial_values.at(i).yaw = yaw;

          std::cout << "Overriden Value - x: " <<  initial_estimate.transform.translation.x << " y: " << initial_estimate.transform.translation.y << " z: " <<  initial_estimate.transform.translation.z << " roll: " << roll << " pitch: " <<  pitch << " yaw: " <<  yaw << std::endl;

          }
      }
    }

    opt.optimize(params, data, logger, pub_, true);

    std::cout << "Parameter Offsets:" << std::endl;
    std::cout << opt.getOffsets()->getOffsetYAML() << std::endl;

    // Send result
    auto result = std::make_shared<CalibratePose::Result>();

    for (const auto& free_frame : params.free_frames) {
      geometry_msgs::msg::TransformStamped estimated_offset;
      estimated_offset.header.frame_id = free_frame.name;
      estimated_offset.child_frame_id = "estimated_pose_offset";
      estimated_offset.transform.translation.x = opt.getOffsets()->get(free_frame.name + "_x");
      estimated_offset.transform.translation.y = opt.getOffsets()->get(free_frame.name + "_y");
      estimated_offset.transform.translation.z = opt.getOffsets()->get(free_frame.name + "_z");

      KDL::Rotation r;
      r = robot_calibration::rotation_from_axis_magnitude(opt.getOffsets()->get(free_frame.name + "_a"), opt.getOffsets()->get(free_frame.name + "_b"), opt.getOffsets()->get(free_frame.name + "_c"));
      double x, y, z, w;
      r.GetQuaternion(x, y, z, w);
      estimated_offset.transform.rotation.x = x;
      estimated_offset.transform.rotation.y = y;
      estimated_offset.transform.rotation.z = z;
      estimated_offset.transform.rotation.w = w;

      // create transform objects that we can used to calculated the calibrated_pose from the offset
      tf2::Transform initial_estimate_tf, estimated_pose_tf;
      tf2::fromMsg(initial_estimate.transform, initial_estimate_tf);
      tf2::fromMsg(estimated_offset.transform, estimated_pose_tf);
      // calculate the final estimated pose by adding in the offset
      estimated_pose_tf = initial_estimate_tf * estimated_pose_tf;
      geometry_msgs::msg::TransformStamped calibrated_pose;
      calibrated_pose.header.frame_id = params.base_link;
      calibrated_pose.child_frame_id = free_frame.name;
      tf2::toMsg(estimated_pose_tf, calibrated_pose.transform);

      result->calibrated_poses.push_back(calibrated_pose);
    }
    RCLCPP_INFO(logger, "Goal succeeded. Send the result");
    goal_handle->succeed(result);
  }

  RCLCPP_INFO(logger, "Done calibrating");
}
