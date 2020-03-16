/*
 * Copyright 2018 Giuseppe Silano, University of Sannio in Benevento, Italy
 * Copyright 2018 Emanuele Aucone, University of Sannio in Benevento, Italy
 * Copyright 2018 Benjamin Rodriguez, MIT, USA
 * Copyright 2018 Luigi Iannelli, University of Sannio in Benevento, Italy
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rotors_control/mellinger_controller.h"
#include "rotors_control/transform_datatypes.h"
#include "rotors_control/Matrix3x3.h"
#include "rotors_control/Quaternion.h" 
#include "rotors_control/stabilizer_types.h"
#include "rotors_control/sensfusion6.h"

#include <math.h> 
#include <ros/ros.h>
#include <time.h>
#include <chrono>

#include <nav_msgs/Odometry.h>
#include <ros/console.h>

#define GRAVITY                                  9.81 /* g [m/s^2]*/
#define M_PI                                     3.14159265358979323846  /* pi [rad]*/
#define OMEGA_OFFSET                             6874  /* OMEGA OFFSET [PWM]*/
#define ANGULAR_MOTOR_COEFFICIENT                0.2685 /* ANGULAR_MOTOR_COEFFICIENT */
#define MOTORS_INTERCEPT                         426.24 /* MOTORS_INTERCEPT [rad/s]*/
#define MAX_PROPELLERS_ANGULAR_VELOCITY          2618 /* MAX PROPELLERS ANGULAR VELOCITY [rad/s]*/
#define MAX_R_DESIDERED                          3.4907 /* MAX R DESIDERED VALUE [rad/s]*/   
#define MAX_THETA_COMMAND                        0.5236 /* MAX THETA COMMMAND [rad]*/
#define MAX_PHI_COMMAND                          0.5236 /* MAX PHI COMMAND [rad]*/
#define MAX_POS_DELTA_OMEGA                      1289 /* MAX POSITIVE DELTA OMEGA [PWM]*/
#define MAX_NEG_DELTA_OMEGA                      -1718 /* MAX NEGATIVE DELTA OMEGA [PWM]*/
#define SAMPLING_TIME                            0.01 /* SAMPLING TIME [s] */

namespace rotors_control{

MellingerController::MellingerController()
    : controller_active_(false),
    state_estimator_active_(false),
    error_x(0),
    error_y(0),
    error_z(0){

      // The control variables are initialized to zero
      control_t_.roll = 0;
      control_t_.pitch = 0;
      control_t_.yawRate = 0;
      control_t_.thrust = 0;

      state_.angularAcc.x = 0; // Angular Acceleration x
      state_.angularAcc.y = 0; // Angular Acceleration y
      state_.angularAcc.z = 0; // Angular Acceleration z

      state_.attitude.roll = 0; // Roll
      state_.attitude.pitch = 0; // Pitch
      state_.attitude.yaw = 0; // Yaw

      state_.position.x = 0; // Position.x
      state_.position.y = 0; // Position.y
      state_.position.z = 0; // Position.z

      state_.angularVelocity.x = 0; // Angular velocity x
      state_.angularVelocity.y = 0; // Angular velocity y
      state_.angularVelocity.z = 0; // Angular velocity z

      state_.linearVelocity.x = 0; //Linear velocity x
      state_.linearVelocity.y = 0; //Linear velocity y
      state_.linearVelocity.z = 0; //Linear velocity z

      state_.attitudeQuaternion.x = 0; // Quaternion x
      state_.attitudeQuaternion.y = 0; // Quaternion y
      state_.attitudeQuaternion.z = 0; // Quaternion z
      state_.attitudeQuaternion.w = 0; // Quaternion w


}

MellingerController::~MellingerController() {}

// Controller gains are entered into local global variables
void MellingerController::SetControllerGains(){

        hover_xyz_stiff_kp_ = Eigen::Vector3f(controller_parameters_.hover_xyz_stiff_kp_.x(),
                                            controller_parameters_.hover_xyz_stiff_kp_.y(),
                                            controller_parameters_.hover_xyz_stiff_kp_.z());
        hover_xyz_soft_kp_  = Eigen::Vector3f(controller_parameters_.hover_xyz_soft_kp_.x(),
                                            controller_parameters_.hover_xyz_soft_kp_.y(),
                                            controller_parameters_.hover_xyz_soft_kp_.z());

        hover_xyz_stiff_ki_ = Eigen::Vector3f(controller_parameters_.hover_xyz_stiff_ki_.x(),
                                            controller_parameters_.hover_xyz_stiff_ki_.y(),
                                            controller_parameters_.hover_xyz_stiff_ki_.z());
        hover_xyz_soft_ki_ = Eigen::Vector3f(controller_parameters_.hover_xyz_soft_ki_.x(),
                                            controller_parameters_.hover_xyz_soft_ki_.y(),
                                            controller_parameters_.hover_xyz_soft_ki_.z());

        hover_xyz_stiff_kd_ = Eigen::Vector3f(  controller_parameters_.hover_xyz_stiff_kd_.x(),
                                                controller_parameters_.hover_xyz_stiff_kd_.y(),
                                                controller_parameters_.hover_xyz_stiff_kd_.z());
        hover_xyz_soft_kd_ = Eigen::Vector3f(   controller_parameters_.hover_xyz_soft_kd_.x(),
                                                controller_parameters_.hover_xyz_soft_kd_.y(),
                                                controller_parameters_.hover_xyz_soft_kd_.z());

        hover_xyz_stiff_angle_kp_ = Eigen::Vector3f(    controller_parameters_.hover_xyz_stiff_angle_kp_.x(),
                                                        controller_parameters_.hover_xyz_stiff_angle_kp_.y(),
                                                        controller_parameters_.hover_xyz_stiff_angle_kp_.z());

        hover_xyz_soft_angle_kp_ = Eigen::Vector3f(     controller_parameters_.hover_xyz_soft_angle_kp_.x(),
                                                        controller_parameters_.hover_xyz_soft_angle_kp_.y(),
                                                        controller_parameters_.hover_xyz_soft_angle_kp_.z());

        hover_xyz_stiff_angle_kd_ = Eigen::Vector3f(    controller_parameters_.hover_xyz_stiff_angle_kd_.x(),
                controller_parameters_.hover_xyz_stiff_angle_kd_.y(),
                controller_parameters_.hover_xyz_stiff_angle_kd_.z());

        hover_xyz_soft_angle_kd_ = Eigen::Vector3f(     controller_parameters_.hover_xyz_soft_angle_kd_.x(),
                controller_parameters_.hover_xyz_soft_angle_kd_.y(),
                controller_parameters_.hover_xyz_soft_angle_kd_.z());

        attitude_kp_ = Eigen::Vector3f(controller_parameters_.attitude_kp_.x(),
                                        controller_parameters_.attitude_kp_.y(),
                                         controller_parameters_.attitude_kp_.z());
        attitude_kd_ = Eigen::Vector3f(controller_parameters_.attitude_kd_.x(),
                                controller_parameters_.attitude_kd_.y(),
                                controller_parameters_.attitude_kd_.z());

        path_angle_kp_ = Eigen::Vector3f( controller_parameters_.path_angle_kp_.x(),
                controller_parameters_.path_angle_kp_.y(),
                controller_parameters_.path_angle_kp_.z());
        path_angle_kd_ = Eigen::Vector3f( controller_parameters_.path_angle_kd_.x(),
                controller_parameters_.path_angle_kd_.y(),
                controller_parameters_.path_angle_kd_.z());

        path_kp_ = Eigen::Vector3f(controller_parameters_.path_angle_kd_.x(),
                controller_parameters_.path_kp_.y(),
                controller_parameters_.path_kp_.z());
        path_kd_ = Eigen::Vector3f(controller_parameters_.path_kd_.x(),
                controller_parameters_.path_kd_.y(),
                controller_parameters_.path_kd_.z());

        bm = controller_parameters_.bm;
        bf = controller_parameters_.bf;
        l = controller_parameters_.l;

        Eigen::Matrix4d Conversion_fw;
        Conversion_fw << bf, bf, bf, bf,
                         0.0, bf*l, 0.0, -bf*l,
                         -bf*l, 0.0, bf*l, 0.0,
                         bm, bm, bm, bm;
        Conversion = Conversion_fw.inverse();

}

void MellingerController::SetTrajectoryPoint(const mav_msgs::EigenTrajectoryPoint& command_trajectory) {
    command_trajectory_= command_trajectory;
    controller_active_= true;
}

void MellingerController::CalculateRotorVelocities(Eigen::Vector4d* rotor_velocities) {
    assert(rotor_velocities);
    
    // This is to disable the controller if we do not receive a trajectory
    if(!controller_active_){
       *rotor_velocities = Eigen::Vector4d::Zero(rotor_velocities->rows());
    return;
    }
    
    double PWM_1, PWM_2, PWM_3, PWM_4;
    ControlMixer(&PWM_1, &PWM_2, &PWM_3, &PWM_4);

    Eigen::Vector4d forces, omega;
    omega = Conversion*forces;
    omega(0) = std::sqrt(omega(0));
    omega(1) = std::sqrt(omega(1));
    omega(2) = std::sqrt(omega(2));
    omega(3) = std::sqrt(omega(3));

    //The omega values are saturated considering physical constraints of the system
    if(!(omega(0) < MAX_PROPELLERS_ANGULAR_VELOCITY && omega(0) > 0))
        if(omega(0) > MAX_PROPELLERS_ANGULAR_VELOCITY)
           omega(0) = MAX_PROPELLERS_ANGULAR_VELOCITY;
        else
           omega(0) = 0;

    if(!(omega(2) < MAX_PROPELLERS_ANGULAR_VELOCITY && omega(2) > 0))
        if(omega(2) > MAX_PROPELLERS_ANGULAR_VELOCITY)
           omega(2) = MAX_PROPELLERS_ANGULAR_VELOCITY;
        else
           omega(2) = 0;

    if(!(omega(3) < MAX_PROPELLERS_ANGULAR_VELOCITY && omega(3) > 0))
        if(omega(3) > MAX_PROPELLERS_ANGULAR_VELOCITY)
           omega(3) = MAX_PROPELLERS_ANGULAR_VELOCITY;
        else
           omega(3) = 0;

    if(!(omega(4) < MAX_PROPELLERS_ANGULAR_VELOCITY && omega(4) > 0))
        if(omega(4) > MAX_PROPELLERS_ANGULAR_VELOCITY)
           omega(4) = MAX_PROPELLERS_ANGULAR_VELOCITY;
        else
           omega(4) = 0;

    ROS_DEBUG("Omega_1: %f Omega_2: %f Omega_3: %f Omega_4: %f", omega(1), omega(2), omega(3), omega(4));
    *rotor_velocities = Eigen::Vector4d(omega(1), omega(2), omega(3), omega(4));
}

void MellingerController::Quaternion2Euler(double* roll, double* pitch, double* yaw) const {
    assert(roll);
    assert(pitch);
    assert(yaw);

    // The estimated quaternion values
    double x, y, z, w;
    x = state_.attitudeQuaternion.x;
    y = state_.attitudeQuaternion.y;
    z = state_.attitudeQuaternion.z;
    w = state_.attitudeQuaternion.w;
    
    tf::Quaternion q(x, y, z, w);
    tf::Matrix3x3 m(q);
    m.getRPY(*roll, *pitch, *yaw);

    ROS_DEBUG("Roll: %f, Pitch: %f, Yaw: %f", *roll, *pitch, *yaw);

}

void MellingerController::ControlMixer(double* PWM_1, double* PWM_2, double* PWM_3, double* PWM_4) {
    assert(PWM_1);
    assert(PWM_2);
    assert(PWM_3);
    assert(PWM_4);
   /*
    if(!state_estimator_active_)
       // When the state estimator is disable, the delta_omega_ value is computed as soon as the new odometry message is available.
       //The timing is managed by the publication of the odometry topic
       HoveringController(&control_t_.thrust);
    */
    // Control signals are sent to the on board control architecture if the state estimator is active
    double delta_phi, delta_theta, delta_psi;
  /*  if(state_estimator_active_){
       crazyflie_onboard_controller_.SetControlSignals(control_t_);
       crazyflie_onboard_controller_.SetDroneState(state_);
       crazyflie_onboard_controller_.RateController(&delta_phi, &delta_theta, &delta_psi);
    }
    else
       RateController(&delta_phi, &delta_theta, &delta_psi);
  */
    *PWM_1 = control_t_.thrust - (delta_theta/2) - (delta_phi/2) - delta_psi;
    *PWM_2 = control_t_.thrust + (delta_theta/2) - (delta_phi/2) + delta_psi;
    *PWM_3 = control_t_.thrust + (delta_theta/2) + (delta_phi/2) - delta_psi;
    *PWM_4 = control_t_.thrust - (delta_theta/2) + (delta_phi/2) + delta_psi;

    ROS_DEBUG("Omega: %f, Delta_theta: %f, Delta_phi: %f, delta_psi: %f", control_t_.thrust, delta_theta, delta_phi, delta_psi);
    ROS_DEBUG("PWM1: %f, PWM2: %f, PWM3: %f, PWM4: %f", *PWM_1, *PWM_2, *PWM_3, *PWM_4);
}

void MellingerController::XYController(double* theta_command, double* phi_command) {
    assert(theta_command);
    assert(phi_command);    
/*
    double v, u;
    u = state_.linearVelocity.x;  
    v = state_.linearVelocity.y;

    double xe, ye;
    ErrorBodyFrame(&xe, &ye);

    double e_vx, e_vy;
    e_vx = xe - u;
    e_vy = ye - v;

    double theta_command_kp;
    theta_command_kp = xy_gain_kp_.x() * e_vx;
    theta_command_ki_ = theta_command_ki_ + (xy_gain_ki_.x() * e_vx * SAMPLING_TIME);
    *theta_command  = theta_command_kp + theta_command_ki_;

    double phi_command_kp;
    phi_command_kp = xy_gain_kp_.y() * e_vy;
    phi_command_ki_ = phi_command_ki_ + (xy_gain_ki_.y() * e_vy * SAMPLING_TIME);
    *phi_command  = phi_command_kp + phi_command_ki_;

    // Theta command is saturated considering the aircraft physical constraints
    if(!(*theta_command < MAX_THETA_COMMAND && *theta_command > -MAX_THETA_COMMAND))
       if(*theta_command > MAX_THETA_COMMAND)
          *theta_command = MAX_THETA_COMMAND;
       else
          *theta_command = -MAX_THETA_COMMAND;

    // Phi command is saturated considering the aircraft physical constraints
    if(!(*phi_command < MAX_PHI_COMMAND && *phi_command > -MAX_PHI_COMMAND))
       if(*phi_command > MAX_PHI_COMMAND)
          *phi_command = MAX_PHI_COMMAND;
       else
          *phi_command = -MAX_PHI_COMMAND;

     ROS_DEBUG("Theta_kp: %f, Theta_ki: %f", theta_command_kp, theta_command_ki_);
     ROS_DEBUG("Phi_kp: %f, Phi_ki: %f", phi_command_kp, phi_command_ki_);
     ROS_DEBUG("Phi_c: %f, Theta_c: %f", *phi_command, *theta_command);
     ROS_DEBUG("E_vx: %f, E_vy: %f", e_vx, e_vy);
     ROS_DEBUG("E_x: %f, E_y: %f", xe, ye);

*/
}

void MellingerController::YawPositionController(double* r_command) {
    assert(r_command);

    /*
    double roll, pitch, yaw;
    Quaternion2Euler(&roll, &pitch, &yaw);   


    double yaw_error, yaw_reference;
    yaw_reference = command_trajectory_.getYaw();
    yaw_error = yaw_reference - yaw;

    double r_command_kp;
    r_command_kp = yaw_gain_kp_ * yaw_error;
    r_command_ki_ = r_command_ki_ + (yaw_gain_ki_ * yaw_error * SAMPLING_TIME);
    *r_command = r_command_ki_ + r_command_kp;

   // R command value is saturated considering the aircraft physical constraints
   if(!(*r_command < MAX_R_DESIDERED && *r_command > -MAX_R_DESIDERED))
      if(*r_command > MAX_R_DESIDERED)
         *r_command = MAX_R_DESIDERED;
      else
         *r_command = -MAX_R_DESIDERED;
  */
}


    void MellingerController::ErrorBodyFrame(double* x_error_, double* y_error_,double* z_error_) const {
        assert(x_error_);
        assert(y_error_);
        assert(z_error_);

        // X and Y reference coordinates
        double x_r = command_trajectory_.position_W[0];
        double y_r = command_trajectory_.position_W[1];
        double z_r = command_trajectory_.position_W[2];

        // Position error
        *x_error_ = x_r - state_.position.x;
        *y_error_ = y_r - state_.position.y;
        *z_error_ = z_r - state_.position.z;
    }


    void MellingerController::ErrorBodyFrame(double* x_error_, double* y_error_,double* z_error_, Eigen::Vector3d &velocity_error) const {
        assert(x_error_);
        assert(y_error_);
        assert(z_error_);

        ErrorBodyFrame(x_error_,y_error_,z_error_);

        Eigen::Vector3d velocity = Eigen::Vector3d(state_.linearVelocity.x, state_.linearVelocity.y,state_.linearVelocity.z);

        velocity_error = command_trajectory_.velocity_W - velocity;

    }


void MellingerController::PathFollowing3D(double* acc_x,double* acc_y, double* acc_z) {
    assert(omega);

    double x_error_, y_error_, z_error_;
    Eigen::Vector3d ev;
    ErrorBodyFrame(&x_error_,&y_error_,&z_error_,ev);

    Eigen::Vector3d position_error = Eigen::Vector3d(x_error_,y_error_,z_error_);
    Eigen::Vector3d norm = position_error/position_error.norm();
    Eigen::Vector3d bnorm = position_error.cross(norm);

    Eigen::Vector3d ep = position_error.dot(norm)*norm + position_error.dot(bnorm)*bnorm;

    *acc_x = path_kp_.x() * ep(0) + path_kd_.x() * ev(0) + command_trajectory_.acceleration_W(0);
    *acc_y = path_kp_.y() * ep(1) + path_kd_.y() * ev(1) + command_trajectory_.acceleration_W(1);
    *acc_z =  path_kp_.z() * ep(2) + path_kd_.z() * ev(2) + command_trajectory_.acceleration_W(2);

}


/* FROM HERE THE FUNCTIONS EMPLOYED WHEN THE STATE ESTIMATOR IS UNABLE ARE REPORTED */

//Such function is invoked by the position controller node when the state estimator is not in the loop
void MellingerController::SetOdometryWithoutStateEstimator(const EigenOdometry& odometry) {
    
    odometry_ = odometry; 

    // Such function is invoked when the ideal odometry sensor is employed
    SetSensorData();
}

// Odometry values are put in the state structure. The structure contains the aircraft state
void MellingerController::SetSensorData() {
    
    // Only the position sensor is ideal, any virtual sensor or systems is available to get it
    state_.position.x = odometry_.position[0];
    state_.position.y = odometry_.position[1];
    state_.position.z = odometry_.position[2];

    state_.linearVelocity.x = odometry_.velocity[0];
    state_.linearVelocity.y = odometry_.velocity[1];
    state_.linearVelocity.z = odometry_.velocity[2];

    state_.attitudeQuaternion.x = odometry_.orientation.x();
    state_.attitudeQuaternion.y = odometry_.orientation.y();
    state_.attitudeQuaternion.z = odometry_.orientation.z();
    state_.attitudeQuaternion.w = odometry_.orientation.w();

    state_.angularVelocity.x = odometry_.angular_velocity[0];
    state_.angularVelocity.y = odometry_.angular_velocity[1];
    state_.angularVelocity.z = odometry_.angular_velocity[2];
}

void MellingerController::HoverControl( double* acc_x, double* acc_y, double* acc_z) {
    assert(acc_x);
    assert(acc_y);
    assert(acc_z);

    double error_px, error_py, error_pz;
    ErrorBodyFrame(&error_px,&error_py,&error_pz);

    bool bho = true;

    if(bho == true)
    {
        // Stiff hover
        error_x = error_x + (hover_xyz_soft_ki_.x() * error_px * SAMPLING_TIME);
        error_y = error_y + (hover_xyz_soft_ki_.y() * error_py * SAMPLING_TIME);
        error_z = error_z + (hover_xyz_soft_ki_.z() * error_pz * SAMPLING_TIME);

        error_px = hover_xyz_stiff_kp_.x() * error_px;
        error_py = hover_xyz_stiff_kp_.y() * error_py;
        error_pz = hover_xyz_stiff_kp_.z() * error_pz;


        *acc_x = error_px + error_x - hover_xyz_stiff_kd_.x() * state_.linearVelocity.x;
        *acc_y = error_py + error_y - hover_xyz_stiff_kd_.y() * state_.linearVelocity.y;
        *acc_z = error_pz + error_z - hover_xyz_stiff_kd_.z() * state_.linearVelocity.z;
    }
    else {

        // Soft hover
        error_x = error_x + (hover_xyz_soft_ki_.x() * error_px * SAMPLING_TIME);
        error_y = error_y + (hover_xyz_soft_ki_.y() * error_py * SAMPLING_TIME);
        error_z = error_z + (hover_xyz_soft_ki_.z() * error_pz * SAMPLING_TIME);

        error_px = hover_xyz_soft_kp_.x() * error_px;
        error_py = hover_xyz_soft_kp_.y() * error_py;
        error_pz = hover_xyz_soft_kp_.z() * error_pz;


        *acc_x = error_px + error_x - hover_xyz_soft_kd_.x() * state_.linearVelocity.x;
        *acc_y = error_py + error_y - hover_xyz_soft_kd_.y() * state_.linearVelocity.y;
        *acc_z = error_pz + error_z - hover_xyz_soft_kd_.z() * state_.linearVelocity.z;
    }


}

void MellingerController::RPThrustControl(double &phi_des, double &theta_des,double &delta_F)
{
    double mass = controller_parameters_.mass;

    phi_des = (c_a.x * sin(command_trajectory_.getYaw()) - c_a.y * cos(command_trajectory_.getYaw()));
    theta_des = (c_a.x * cos(command_trajectory_.getYaw()) + c_a.y * sin(command_trajectory_.getYaw()));
    delta_F = mass * (c_a.z + GRAVITY);

}


void MellingerController::AttitudeController(double* p_command, double* q_command) {
    assert(p_command);
    assert(q_command);

    Eigen::Vector3f errorAngle;
    Eigen::Vector3f errorAngularVelocity;

    AttitudeError(errorAngle ,errorAngularVelocity);

    double delta_roll, delta_pitch, delta_yaw;

    delta_roll = attitude_kp_.x() * errorAngle.x() + attitude_kd_.x() * errorAngle.x();
    delta_pitch = attitude_kp_.y() * errorAngle.y() + attitude_kd_.y() * errorAngle.y();
    delta_yaw = attitude_kp_.z() * errorAngle.z() + attitude_kd_.z() * errorAngle.z();

    ROS_DEBUG("Phi_c: %f, Phi_e: %f, Theta_c: %f, Theta_e: %f", phi_command, phi_error, theta_command, theta_error);


}

void MellingerController::AttitudeError(Eigen::Vector3f &errorAngle ,Eigen::Vector3f &errorAngularVelocity)
{
        double roll, pitch, yaw;
        Quaternion2Euler(&roll, &pitch, &yaw);

        double roll_des, pitch_des, yaw_des;
        tf::Quaternion q( command_trajectory_.orientation_W_B.x(),
                          command_trajectory_.orientation_W_B.y(),
                          command_trajectory_.orientation_W_B.z(),
                          command_trajectory_.orientation_W_B.w());
        tf::Matrix3x3 m(q);
        m.getRPY(roll_des, pitch_des, yaw_des);

        errorAngle.x() =   roll_des - roll;
        errorAngle.y() =   pitch_des - pitch;
        errorAngle.z() =   yaw_des - yaw;

        errorAngularVelocity.x() = command_trajectory_.angular_velocity_W(0) - state_.angularVelocity.x;
        errorAngularVelocity.y() = command_trajectory_.angular_velocity_W(1) - state_.angularVelocity.y;
        errorAngularVelocity.z() = command_trajectory_.angular_velocity_W(2) - state_.angularVelocity.z;

}



/* FROM HERE THE FUNCTIONS EMPLOYED WHEN THE STATE ESTIMATOR IS ABLED ARE REPORTED */

// Such function is invoked by the position controller node when the state estimator is considered in the loop
void MellingerController::SetOdometryWithStateEstimator(const EigenOdometry& odometry) {
    
    odometry_ = odometry;    
}


// The aircraft attitude is computed by the complementary filter with a frequency rate of 250Hz
void MellingerController::CallbackAttitudeEstimation() {

    // Angular velocities updating
    complementary_filter_crazyflie_.EstimateAttitude(&state_, &sensors_);

    ROS_DEBUG("Attitude Callback");

}

// The high level control runs with a frequency of 100Hz
void MellingerController::CallbackHightLevelControl() {

    /*
    // Thrust value
    HoveringController(&control_t_.thrust);
    
    // Phi and theta command signals. The Error Body Controller is invoked every 0.01 seconds. It uses XYController's outputs
    XYController(&control_t_.pitch, &control_t_.roll);

    // Yaw rate command signals
    YawPositionController(&control_t_.yawRate);
   
    ROS_DEBUG("Position_x: %f, Position_y: %f, Position_z: %f", state_.position.x, state_.position.y, state_.position.z);

    ROS_DEBUG("Angular_velocity_x: %f, Angular_velocity_y: %f, Angular_velocity_z: %f", state_.angularVelocity.x, 
             state_.angularVelocity.y, state_.angularVelocity.z);

    ROS_DEBUG("Linear_velocity_x: %f, Linear_velocity_y: %f, Linear_velocity_z: %f", state_.linearVelocity.x, 
             state_.linearVelocity.y, state_.linearVelocity.z);
*/
}

// The aircraft angular velocities are update with a frequency of 500Hz
void MellingerController::SetSensorData(const sensorData_t& sensors) {
  
    // The functions runs at 500Hz, the same frequency with which the IMU topic publishes new values (with a frequency of 500Hz)
    sensors_ = sensors;
    complementary_filter_crazyflie_.EstimateRate(&state_, &sensors_);
    
    if(!state_estimator_active_)
        state_estimator_active_= true;
    
    // Only the position sensor is ideal, any virtual sensor or systems is available to get these data
    // Every 0.002 seconds the odometry message values are copied in the state_ structure, but they change only 0.01 seconds
    state_.position.x = odometry_.position[0];
    state_.position.y = odometry_.position[1];
    state_.position.z = odometry_.position[2];

    state_.linearVelocity.x = odometry_.velocity[0];
    state_.linearVelocity.y = odometry_.velocity[1];
    state_.linearVelocity.z = odometry_.velocity[2];
}


}
