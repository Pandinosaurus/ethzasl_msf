/*
 * Copyright (C) 2012-2013 Simon Lynen, ASL, ETH Zurich, Switzerland
 * You can contact the author at <slynen at ethz dot ch>
 * Copyright (C) 2011-2012 Stephan Weiss, ASL, ETH Zurich, Switzerland
 * You can contact the author at <stephan dot weiss at ieee dot org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef FLOW_MEASUREMENT_HPP_
#define FLOW_MEASUREMENT_HPP_

#include <arkflow_ros/OpticalFlow.h>
#include <msf_core/eigen_utils.h>
#include <msf_core/msf_core.h>
#include <msf_core/msf_measurement.h>

namespace msf_updates {
namespace flow_measurement {
enum { nMeasurements = 2 };

typedef msf_core::MSF_Measurement<
    arkflow_ros::OpticalFlow,
    Eigen::Matrix<double, nMeasurements, nMeasurements>, msf_updates::EKFState>
    FlowMeasurementBase;

template <int StateQivIdx = EKFState::StateDefinition_T::q_iv,
          int StatePivIdx = EKFState::StateDefinition_T::p_iv>
struct FlowMeasurement : public FlowMeasurementBase {
 private:
  typedef FlowMeasurementBase Measurement_t;
  typedef Measurement_t::Measurement_ptr measptr_t;

  double z_dt_{0.0};
  double z_range_{0.0};

  virtual void MakeFromSensorReadingImpl(measptr_t msg) {
    Eigen::Matrix<
        double, nMeasurements,
        msf_core::MSF_Core<msf_updates::EKFState>::nErrorStatesAtCompileTime>
        H_old;
    Eigen::Matrix<double, nMeasurements, 1> r_old;

    H_old.setZero();

    // Get measurements
    _z_v =
        Eigen::Matrix<double, 2, 1>(msg->flow_integral_x,
        msg->flow_integral_y);
    z_dt_ = msg->integration_interval;
    z_range_ = msg->range;

    // TODO(clanegge): implement
    if (!_fixed_covariance) {  // Take covariance from sensor.
      MSF_ERROR_STREAM(
          "Covariance from sensor not implemented yet. Using fixed "
          "covariance.");
    }

    // Take fixed covariance from reconfigure GUI
    const double s_zv = _n_zv * _n_zv;

    R_ = (Eigen::Matrix<double, nMeasurements, 1>() << s_zv, s_zv)
             .finished()
             .asDiagonal();
    // TODO(clanegge): DO we need to clear out cross-correlations?
    // std::cout << "Covariance matrix:\n" << R_ << std::endl;
    R_(0, 1) = 0.0;
    R_(1, 0) = 0.0;
  }

 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  Eigen::Matrix<double, nMeasurements, 1> _z_v{
      Eigen::Matrix<double, nMeasurements,
                    1>::Zero()};  /// Velocity measurement in xy
                                  /// sensor coordinates.
  double _n_zv{0.0};              /// Velocity measurement noise.

  bool _fixed_covariance{false};
  int _fixedstates{0};

  typedef msf_updates::EKFState EKFState_T;
  typedef EKFState_T::StateSequence_T StateSequence_T;
  typedef EKFState_T::StateDefinition_T StateDefinition_T;

  enum AuxState { q_iv = StateQivIdx, p_iv = StatePivIdx };

  virtual ~FlowMeasurement() {}
  FlowMeasurement(double n_zv, bool fixed_covariance,
                  bool isabsoluteMeasurement, int sensorID,
                  bool enable_mah_outlier_rejection, double mah_threshold,
                  int fixedstates)
      : FlowMeasurementBase(isabsoluteMeasurement, sensorID,
                            enable_mah_outlier_rejection, mah_threshold),
        _n_zv(n_zv),
        _fixed_covariance(fixed_covariance),
        _fixedstates(fixedstates) {}

  virtual std::string Type() { return "velocity_flow"; }

  virtual void CalculateH(
      shared_ptr<EKFState_T> state_in,
      Eigen::Matrix<double, nMeasurements,
                    msf_core::MSF_Core<EKFState_T>::nErrorStatesAtCompileTime>&
          H) {
    const EKFState_T& state =
        *state_in;  // Get a const ref, so we can read core states.

    H.setZero();

    // TODO(clanegge): Make sure this is the correct way around
    // Get rotation matrices.
    Eigen::Matrix<double, 3, 3> C_vi =
        state.Get<StateQivIdx>().conjugate().toRotationMatrix();

    // Preprocess for elements in H matrix.
    Eigen::Matrix<double, 3, 3> piv_sk = Skew(state.Get<StatePivIdx>());

    // Get indices of states in error vector
    enum {
      kIdxstartcorr_v =
          msf_tmp::GetStartIndexInCorrection<StateSequence_T,
                                             StateDefinition_T::v>::value,
      kIdxstartcorr_bw =
          msf_tmp::GetStartIndexInCorrection<StateSequence_T,
                                             StateDefinition_T::b_w>::value,
    };

    // Read the fixed states flags.
    bool calibposfix = (_fixedstates & 1 << StatePivIdx);
    bool calibattfix = (_fixedstates & 1 << StateQivIdx);

    // TODO(clanegge):
    if (!calibposfix || !calibattfix) {
      MSF_ERROR_STREAM(
          "Online calibration of velocity sensor not implemented yet. Using "
          "fixed transform.");
      calibposfix = true;
      calibattfix = true;
    }

    if (calibposfix) {
      state_in->ClearCrossCov<StatePivIdx>();
    }
    if (calibattfix) {
      state_in->ClearCrossCov<StateQivIdx>();
    }

    // Construct H matrix.
    // velocity:
    // C_vi * i_v_i
    H.block<2, 3>(0, kIdxstartcorr_v) =
        C_vi.block<2, 3>(0, 0) * z_dt_ / z_range_;

    // gyro bias/angular velocity:
    // Cross term C_vi*( [i_omega_i - i_b_w] x r_iv)
    // = C_vi*r_iv_skew*i_b_w
    H.block<2, 3>(0, kIdxstartcorr_bw) =
        (C_vi * piv_sk).block<2, 3>(0, 0) * z_dt_ / z_range_;
  }

  virtual void Apply(shared_ptr<EKFState_T> state_nonconst_new,
                     msf_core::MSF_Core<EKFState_T>& core) {
    const EKFState_T& state = *state_nonconst_new;
    // init variables
    Eigen::Matrix<double, nMeasurements,
                  msf_core::MSF_Core<EKFState_T>::nErrorStatesAtCompileTime>
        H_new;
    Eigen::Matrix<double, nMeasurements, 1> r_old;

    CalculateH(state_nonconst_new, H_new);

    // Get rotation matrices.
    Eigen::Matrix<double, 3, 3> C_vi =
        state.Get<StateQivIdx>().conjugate().toRotationMatrix();

    // 3d Sensor velocity in imu frame:
    Eigen::Matrix<double, 3, 1> i_v_v =
        (state.Get<StateDefinition_T::v>() +
         (state.w_m - state.Get<StateDefinition_T::b_w>())
             .cross(state.Get<StatePivIdx>()));

    // Construct residuals.
    r_old = _z_v - ((C_vi * i_v_v).block<2, 1>(0, 0) * z_dt_ / z_range_);
    // std::cout << _z_v[0] << " - "
    //           << ((C_vi * i_v_v).block<2, 1>(0, 0) * z_dt_ / z_range_)[0]
    //           << " = " << r_old[0] << std::endl;
    // std::cout << _z_v[1] << " - "
    //           << ((C_vi * i_v_v).block<2, 1>(0, 0) * z_dt_ / z_range_)[1]
    //           << " = " << r_old[1] << std::endl;

    if (!CheckForNumeric(r_old, "r_old")) {
      MSF_ERROR_STREAM("r_old: " << r_old);
      MSF_WARN_STREAM(
          "state: "
          << const_cast<EKFState_T&>(state).ToEigenVector().transpose());
    }
    if (!CheckForNumeric(H_new, "H_old")) {
      MSF_ERROR_STREAM("H_old: " << H_new);
      MSF_WARN_STREAM(
          "state: "
          << const_cast<EKFState_T&>(state).ToEigenVector().transpose());
    }
    if (!CheckForNumeric(R_, "R_")) {
      MSF_ERROR_STREAM("R_: " << R_);
      MSF_WARN_STREAM(
          "state: "
          << const_cast<EKFState_T&>(state).ToEigenVector().transpose());
    }

    // Call update step in base class.
    this->CalculateAndApplyCorrection(state_nonconst_new, core, H_new, r_old,
                                      R_);
  }
};

}  // namespace flow_measurement
}  // namespace msf_updates

#endif  // FLOW_MEASUREMENT_HPP_