// Copyright 2020 Sandro Merkli, inspired by Christopher Ho's mpc code
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef RECORDREPLAY_PLANNER_NODE__RECORDREPLAY_PLANNER_NODE_HPP_
#define RECORDREPLAY_PLANNER_NODE__RECORDREPLAY_PLANNER_NODE_HPP_

#include <recordreplay_planner_node/visibility_control.hpp>
#include <recordreplay_planner/recordreplay_planner.hpp>
#include <tf2_msgs/msg/tf_message.hpp>
#include <autoware_auto_msgs/msg/trajectory.hpp>
#include <autoware_auto_msgs/msg/vehicle_kinematic_state.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>

#include <rclcpp/rclcpp.hpp>

#include <string>
#include <memory>

namespace motion
{
namespace planning
{
namespace recordreplay_planner_node
{
using PlannerPtr = std::unique_ptr<motion::planning::recordreplay_planner::RecordReplayPlanner>;
using tf2_msgs::msg::TFMessage;
using State = autoware_auto_msgs::msg::VehicleKinematicState;
using Trajectory = autoware_auto_msgs::msg::Trajectory;
using Transform = geometry_msgs::msg::TransformStamped;

class RECORDREPLAY_PLANNER_NODE_PUBLIC RecordReplayPlannerNode : public rclcpp::Node
{
public:
  /// Parameter file constructor
  RecordReplayPlannerNode(const std::string & name, const std::string & ns);
  /// Explicit constructor
  RecordReplayPlannerNode(
    const std::string & name,
    const std::string & ns,
    const std::string & tf_topic,
    const std::string & trajectory_topic);

protected:
  void set_planner(PlannerPtr && planner) noexcept;

  rclcpp::Subscription<TFMessage>::SharedPtr m_tf_sub{};
  rclcpp::Publisher<Trajectory>::SharedPtr m_trajectory_pub{};
  PlannerPtr m_planner{nullptr};

private:
  RECORDREPLAY_PLANNER_NODE_LOCAL void init(
    const std::string & tf_topic,
    const std::string & trajectory_topic
  );
};  // class RecordReplayPlannerNode
}  // namespace recordreplay_planner_node
}  // namespace planning
}  // namespace motion

#endif  // RECORDREPLAY_PLANNER_NODE__RECORDREPLAY_PLANNER_NODE_HPP_
