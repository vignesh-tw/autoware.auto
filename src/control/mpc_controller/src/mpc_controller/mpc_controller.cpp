// Copyright 2019 Christopher Ho
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

#include "mpc_controller/mpc_controller.hpp"

// Treat as a system header since we don't want to touch that autogenerated stuff..
#include <acado_common.h>
#include <motion_common/motion_common.hpp>
#include <time_utils/time_utils.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

// extern variables in autogenerated code
ACADOworkspace acadoWorkspace;
ACADOvariables acadoVariables;

// TODO(c.ho) gsl::at() everywhere

namespace motion
{
namespace control
{
namespace mpc_controller
{

/// Typedef in namespace scope instead of macro--easier for debugging
using AcadoReal = real_t;

////////////////////////////////////////////////////////////////////////////////

constexpr auto HORIZON = static_cast<std::size_t>(ACADO_N);
// State variable indices
static_assert(ACADO_NX == 4, "Unexpected num of state variables");
constexpr auto NX = static_cast<std::size_t>(ACADO_NX);
constexpr auto IDX_X = 0U;
constexpr auto IDX_Y = 1U;
constexpr auto IDX_HEADING = 2U;
constexpr auto IDX_VEL_LONG = 3U;
// Control variable indices
static_assert(ACADO_NU == 2, "Unexpected num of control variables");
constexpr auto NU = static_cast<std::size_t>(ACADO_NU);
constexpr auto IDX_JERK = 0U;
constexpr auto IDX_WHEEL_ANGLE_RATE = 1U;

constexpr std::chrono::nanoseconds MpcController::solver_time_step;

////////////////////////////////////////////////////////////////////////////////
MpcController::MpcController(const Config & config)
: ControllerBase{config.behavior()},
  m_config{config},
  m_computed_trajectory{rosidl_runtime_cpp::MessageInitialization::ALL},
  m_last_reference_index{}
{
  if (config.do_interpolate()) {
    m_interpolated_trajectory = std::make_unique<Trajectory>();
    m_interpolated_trajectory->points.reserve(Trajectory::CAPACITY);
  }
  m_computed_trajectory.points.reserve(Trajectory::CAPACITY);
  acado_initializeSolver();
  apply_config(m_config);
}

////////////////////////////////////////////////////////////////////////////////
const Config & MpcController::get_config() const
{
  return m_config;
}

////////////////////////////////////////////////////////////////////////////////
void MpcController::set_config(const Config & config)
{
  m_config = config;
  ControllerBase::set_base_config(m_config.behavior());
  apply_config(m_config);
}

////////////////////////////////////////////////////////////////////////////////
Command MpcController::compute_command_impl(const State & state)
{
  const auto current_idx = get_current_state_temporal_index();

  auto cold_start = update_references(current_idx);
  const auto dt = x0_time_offset(state, current_idx);
  initial_conditions(predict(state.state, dt));
  {
    static_assert(sizeof(std::size_t) >= sizeof(Index), "static cast might truncate");
    // This HAS to happen after initial conditions; relies on x0 being set
    // In addition, this has to run every iteration, since there's no guarantee of
    // smoothness in the received state
    const auto max_pts = get_reference_trajectory().points.size();
    const auto horizon = std::min(static_cast<std::size_t>(max_pts - current_idx), HORIZON);
    cold_start = ensure_reference_consistency(horizon) || cold_start;
    // Consider different ways of updating initial guess for reference update
  }
  if (cold_start) {
    std::fill(&acadoVariables.u[0U], &acadoVariables.u[HORIZON * NU], AcadoReal{});
    acado_initializeNodesByForwardSimulation();
  }
  // TODO(c.ho) further validation on state
  solve();
  // Get result
  return interpolated_command(dt);
}

////////////////////////////////////////////////////////////////////////////////
void MpcController::solve()
{
  const auto prep_ret = acado_preparationStep();
  if (0 != prep_ret) {
    std::string err_str{"Solver preparation error: ", std::string::allocator_type{}};
    err_str += std::to_string(prep_ret);
    throw std::runtime_error{err_str};
  }
  const auto solve_ret = acado_feedbackStep();
  if (0 != solve_ret) {
    std::string err_str{"Solver error: ", std::string::allocator_type{}};
    err_str += std::to_string(solve_ret);
    throw std::runtime_error{err_str};
  }
}

////////////////////////////////////////////////////////////////////////////////
bool MpcController::update_references(const Index current_idx)
{
  const auto cold_start = Index{} == current_idx;
  // Roll forward previous solutions, references; backfill references or prune weights
  if (!cold_start) {
    const auto advance_idx = current_idx - m_last_reference_index;
    advance_problem(advance_idx);
    const auto max_pts = get_reference_trajectory().points.size();
    if (max_pts - current_idx >= HORIZON) {
      backfill_reference(advance_idx);
    } else {
      const auto receded_horizon = max_pts - current_idx;
      apply_terminal_weights(receded_horizon - 1);
      zero_nominal_weights(receded_horizon, receded_horizon + advance_idx);
      zero_terminal_weights();
    }

    // Set index *before* potential throw statements
    m_last_reference_index = current_idx;
  }
  return cold_start;
}

////////////////////////////////////////////////////////////////////////////////
void MpcController::initial_conditions(const Point & state)
{
  // Set x0
  acadoVariables.x0[IDX_X] = static_cast<AcadoReal>(state.x);
  acadoVariables.x0[IDX_Y] = static_cast<AcadoReal>(state.y);
  acadoVariables.x0[IDX_HEADING] =
    static_cast<AcadoReal>(motion_common::to_angle(state.heading));
  acadoVariables.x0[IDX_VEL_LONG] = static_cast<AcadoReal>(state.longitudinal_velocity_mps);
  // Initialization stuff
  acadoVariables.x[IDX_X] = acadoVariables.x0[IDX_X];
  acadoVariables.x[IDX_Y] = acadoVariables.x0[IDX_Y];
  acadoVariables.x[IDX_HEADING] = acadoVariables.x0[IDX_HEADING];
  acadoVariables.x[IDX_VEL_LONG] = acadoVariables.x0[IDX_VEL_LONG];
}

////////////////////////////////////////////////////////////////////////////////
std::chrono::nanoseconds MpcController::x0_time_offset(const State & state, Index idx)
{
  using time_utils::from_message;
  const auto & traj = get_reference_trajectory();
  // What time stamp of x0 should be
  const auto t0 =
    (from_message(traj.header.stamp) + from_message(traj.points[idx].time_from_start));
  const auto dt = t0 - from_message(state.header.stamp);
  return dt;
}

////////////////////////////////////////////////////////////////////////////////
Command MpcController::interpolated_command(const std::chrono::nanoseconds x0_time_offset)
{
  // If I roll backwards, then the actual now() time is forward
  const auto step = solver_time_step;
  auto dt_ = get_config().control_lookahead_duration() - x0_time_offset;
  const auto max_dt = step * (HORIZON - 1U);
  if (dt_ > max_dt) {
    dt_ = max_dt;
  }
  // Compute count
  // At most second from last for interp
  const auto count = std::min(dt_ / step, static_cast<decltype(dt_)::rep>(HORIZON - 2U));
  const auto dt = dt_ - (count * step);
  using std::chrono::duration_cast;
  using std::chrono::duration;
  const auto t = duration_cast<duration<float>>(dt) / duration_cast<duration<float>>(step);

  Command ret{rosidl_runtime_cpp::MessageInitialization::ALL};
  {
    ret.rear_wheel_angle_rad = {};
    // interpolation
    const auto idx = count * ACADO_NU;
    const auto longitudinal0 = static_cast<Real>(acadoVariables.u[idx + IDX_JERK]);
    const auto lateral0 = static_cast<Real>(acadoVariables.u[idx + IDX_WHEEL_ANGLE_RATE]);
    const auto jdx = (count + 1U) * ACADO_NU;
    const auto longitudinal1 = static_cast<Real>(acadoVariables.u[jdx + IDX_JERK]);
    const auto lateral1 = static_cast<Real>(acadoVariables.u[jdx + IDX_WHEEL_ANGLE_RATE]);
    ret.front_wheel_angle_rad = motion_common::interpolate(lateral0, lateral1, t);
    ret.long_accel_mps2 = motion_common::interpolate(longitudinal0, longitudinal1, t);
    ret.velocity_mps = acadoVariables.x[(idx * NX) + IDX_VEL_LONG];
  }

  if (!std::isfinite(ret.long_accel_mps2) || !std::isfinite(ret.front_wheel_angle_rad) ) {
    throw std::runtime_error{"interpolation failed, result is not finite (NaN/Inf)"};
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
const MpcController::Trajectory & MpcController::get_computed_trajectory() const noexcept
{
  auto & traj = m_computed_trajectory;
  traj.header = get_reference_trajectory().header;
  traj.points.resize(HORIZON);
  for (std::size_t i = {}; i < HORIZON; ++i) {
    auto & pt = traj.points[i];
    const auto idx = NX * i;
    pt.x = static_cast<Real>(acadoVariables.x[idx + IDX_X]);
    pt.y = static_cast<Real>(acadoVariables.x[idx + IDX_Y]);
    pt.longitudinal_velocity_mps = static_cast<Real>(acadoVariables.x[idx + IDX_VEL_LONG]);
    pt.lateral_velocity_mps = Real{};
    const auto heading = static_cast<Real>(acadoVariables.x[idx + IDX_HEADING]);
    pt.heading = motion_common::from_angle(heading);
    const auto jdx = NU * i;
    pt.acceleration_mps2 = static_cast<Real>(acadoVariables.u[jdx + IDX_JERK]);
    pt.heading_rate_rps = static_cast<Real>(acadoVariables.u[jdx + IDX_WHEEL_ANGLE_RATE]);
  }
  return m_computed_trajectory;
}

////////////////////////////////////////////////////////////////////////////////
ControlDerivatives MpcController::get_computed_control_derivatives() const noexcept
{
  // return ControlDerivatives{
  //   static_cast<Real>(acadoVariables.u[IDX_JERK]),
  //   static_cast<Real>(acadoVariables.u[IDX_WHEEL_ANGLE_RATE])};
  return ControlDerivatives{{}, {}};
}

////////////////////////////////////////////////////////////////////////////////
void MpcController::apply_config(const Config & cfg)
{
  apply_parameters(cfg.vehicle_param());
  apply_bounds(cfg.limits());
  apply_weights(cfg.optimization_param());
}

////////////////////////////////////////////////////////////////////////////////
void MpcController::apply_parameters(const VehicleConfig & cfg) noexcept
{
  static_assert(ACADO_NOD == 2U, "Num online parameters is not expected value!");
  for (std::size_t i = {}; i < HORIZON; ++i) {
    const auto idx = i * ACADO_NOD;
    constexpr auto idx_Lf = 0U;
    constexpr auto idx_Lr = 1U;
    acadoVariables.od[idx + idx_Lf] = static_cast<AcadoReal>(cfg.length_cg_front_axel());
    acadoVariables.od[idx + idx_Lr] = static_cast<AcadoReal>(cfg.length_cg_rear_axel());
  }
}
////////////////////////////////////////////////////////////////////////////////
void MpcController::apply_bounds(const LimitsConfig & cfg) noexcept
{
  static_assert(ACADO_HARDCODED_CONSTRAINT_VALUES == 0, "Constraints not hard coded");
  constexpr auto NUM_CTRL_CONSTRAINTS = 2U;
  constexpr auto NUM_STATE_CONSTRAINTS = 1U;
  for (std::size_t i = {}; i < HORIZON; ++i) {
    {
      const auto idx = i * NUM_CTRL_CONSTRAINTS;
      constexpr auto idx_ax = 0U;
      constexpr auto idx_delta = 1U;
      acadoVariables.lbValues[idx + idx_ax] = static_cast<AcadoReal>(cfg.acceleration().min());
      acadoVariables.ubValues[idx + idx_ax] = static_cast<AcadoReal>(cfg.acceleration().max());
      acadoVariables.lbValues[idx + idx_delta] =
        static_cast<AcadoReal>(cfg.steer_angle().min());
      acadoVariables.ubValues[idx + idx_delta] =
        static_cast<AcadoReal>(cfg.steer_angle().max());
    }
    {
      // DifferentialState or Affine constraints are sometimes put into different structs
      // i.e. when using qpOASES
      const auto idx = i * NUM_STATE_CONSTRAINTS;
      // These are different from the general state constraints because not all states
      // have constraints
      // If you're changing this, check the order in the code generation script
      constexpr auto idx_u = 0U;
      acadoVariables.lbAValues[idx + idx_u] =
        static_cast<AcadoReal>(cfg.longitudinal_velocity().min());
      acadoVariables.ubAValues[idx + idx_u] =
        static_cast<AcadoReal>(cfg.longitudinal_velocity().max());
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
bool MpcController::check_new_trajectory(const Trajectory & trajectory) const
{
  // Check that all heading values are valid (i.e. are normalized 2D quaternions)
  if (!motion_common::heading_ok(trajectory)) {
    return false;
  }
  // If interpolating, no checks yet
  if (get_config().do_interpolate()) {
    return true;
  }
  for (Index idx = {}; idx < trajectory.points.size(); ++idx) {
    const auto & pt = trajectory.points[idx];
    // std::chrono::duration::abs is C++17
    const auto t0 = time_utils::from_message(trajectory.header.stamp);
    const auto ref = (idx * solver_time_step) + t0;
    const auto dt = get_config().sample_period_tolerance();
    const auto ub = ref + dt;
    const auto lb = ref - dt;
    const auto t = time_utils::from_message(pt.time_from_start) + t0;
    if ((t < lb) || (t > ub)) {
      return false;
    }
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
std::string MpcController::name() const
{
  static_assert(ACADO_QP_SOLVER == ACADO_QPOASES, "QP solver backend was changed!");
  return std::string{"bicycle mpc controller qpoases", std::allocator<char>{}};
}

////////////////////////////////////////////////////////////////////////////////
Index MpcController::get_compute_iterations() const
{
  // Depends on QP solver backend; qpOASES does not expose this
  return Index{};
}
}  // namespace mpc_controller
}  // namespace control
}  // namespace motion
