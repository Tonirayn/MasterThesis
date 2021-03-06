/*
 * Author : Antonio Marino
 * date: 2020
*/

/*
This controller is based on the following publication:

Daniel Mellinger, Vijay Kumar:
Minimum snap trajectory generation and control for quadrotors.
IEEE International Conference on Robotics and Automation (ICRA), 2011.

Wu, Guofan, and Koushil Sreenath:
Variation-Based Linearization of Nonlinear Systems Evolving
on SO (3) and S^{2}" IEEE Access 3 (2015): 1592-1604.

it has been added the following:
 * Integral terms (compensates for: battery voltage drop over time, unbalanced center of mass due to asymmmetries, and uneven wear on propellers and motors)
 * D-term for angular velocity
 * Support to use this controller as an attitude-only controller for manual flight



*/

#include <math.h>

#include "param.h"
#include "log.h"
#include "math3d.h"
#include "position_controller.h"
#include "controller_aggressive.h"
#include "gainsloader.h"

#define GRAVITY_MAGNITUDE (9.81f)

static float g_vehicleMass = 0.032; // TODO: should be CF global for other modules
static float massThrust = 132000;

static float kf = 14580000;
static float kf_l = 316956521;
static float km = 3269600000;

// XY Position PID
static float kp_xy = 0.4;       // P
static float kd_xy = 0.2;       // D
static float ki_xy = 0.05;      // I
static float i_range_xy = 2.0;

// Z Position
static float kp_z = 1.25;       // P
static float kd_z = 0.4;        // D
static float ki_z = 0.05;       // I
static float i_range_z  = 0.4;

// Attitude
static float kR_xy = 70000; // P
static float kw_xy = 20000; // D
static float ki_m_xy = 0.0; // I
static float i_range_m_xy = 1.0;

// Yaw
static float kR_z = 60000; // P
static float kw_z = 12000; // D
static float ki_m_z = 500; // I
static float i_range_m_z  = 1500;

// roll and pitch angular velocity
static float kd_omega_rp = 200; // D


static float stateAttitudeRateRoll;
static float stateAttitudeRatePitch;


// Helper variables
static float i_error_x = 0;
static float i_error_y = 0;
static float i_error_z = 0;

static float prev_omega_roll;
static float prev_omega_pitch;
static float prev_setpoint_omega_roll;
static float prev_setpoint_omega_pitch;

static float i_error_m_x = 0;
static float i_error_m_y = 0;
static float i_error_m_z = 0;

// Logging variables
static struct vec z_axis_desired;
static float roll_desired; // deg
static float pitch_desired; // deg

static float rollddes;
static float dt;

static gain_t gain_setpoint;

void controllerAggressiveReset(void)
{
  i_error_x = 0;
  i_error_y = 0;
  i_error_z = 0;
  i_error_m_x = 0;
  i_error_m_y = 0;
  i_error_m_z = 0;
}

double switchingbellshape(float x,float xmin,float xmax)
{
	float alpha;

	if (x <= xmin){
			alpha = 0;
		}
		else if (x >= xmax){
			alpha = 1;
		}
		else{
			float cosarg = (x - xmin) * M_PI_F / (xmax - xmin) + M_PI_F;
			alpha = (0.5 * cos(cosarg) + 0.5);
		}


	return  alpha;
}


void controllerAggressiveInit(void)
{
  controllerAggressiveReset();
}

bool controllerAggressiveTest(void)
{
  return true;
}

static float clamp(float value, float min, float max) {
  if (value < min) return min;
  if (value > max) return max;
  return value;
}

void controllerAggressive(control_t *control, setpoint_t *setpoint,
                                         const sensorData_t *sensors,
                                         const state_t *state,
                                         const uint32_t tick)
{

  loaderGetGains(&gain_setpoint);

  struct vec r_error;
  struct vec v_error;
  struct vec target_thrust;
  struct vec z_axis;
  float current_thrust, current_thrust_a;
  struct vec x_axis_desired;
  struct vec y_axis_desired;
  struct vec x_c_des;
  struct vec eR, eRa, ew, M, Ma;
  float desiredYaw = 0; //deg

  if (!RATE_DO_EXECUTE(ATTITUDE_RATE, tick)) {
    return;
  }

  float cond = vmag(mkvec(radians(setpoint->attitudeRate.roll),radians(setpoint->attitudeRate.pitch),radians(setpoint->attitudeRate.yaw)));

  float alpha = switchingbellshape(cond,5,5);


  dt = (float)(1.0f/ATTITUDE_RATE);
  struct vec setpointPos = mkvec(setpoint->position.x, setpoint->position.y, setpoint->position.z);
  struct vec setpointVel = mkvec(setpoint->velocity.x, setpoint->velocity.y, setpoint->velocity.z);
  struct vec statePos = mkvec(state->position.x, state->position.y, state->position.z);
  struct vec stateVel = mkvec(state->velocity.x, state->velocity.y, state->velocity.z);

  // Position Error (ep)
  r_error = vsub(setpointPos, statePos);

  // Velocity Error (ev)
  v_error = vsub(setpointVel, stateVel);

  // Integral Error
  i_error_z += r_error.z * dt;
  i_error_z = clamp(i_error_z, -i_range_z, i_range_z);

  i_error_x += r_error.x * dt;
  i_error_x = clamp(i_error_x, -i_range_xy, i_range_xy);

  i_error_y += r_error.y * dt;
  i_error_y = clamp(i_error_y, -i_range_xy, i_range_xy);

  struct quat q = mkquat(state->attitudeQuaternion.x, state->attitudeQuaternion.y, state->attitudeQuaternion.z, state->attitudeQuaternion.w);
  struct mat33 R = quat2rotmat(q);

  // [ew]
  float err_d_roll = 0;
  float err_d_pitch = 0;

  stateAttitudeRateRoll = radians(sensors->gyro.x);
  stateAttitudeRatePitch = -radians(sensors->gyro.y);
  float stateAttitudeRateYaw = radians(sensors->gyro.z);

  ew.x = radians(setpoint->attitudeRate.roll) - stateAttitudeRateRoll;
  ew.y = -radians(setpoint->attitudeRate.pitch) - stateAttitudeRatePitch;
  ew.z = radians(setpoint->attitudeRate.yaw) - stateAttitudeRateYaw;

  rollddes = radians(setpoint->attitudeRate.roll);

  // Aggressive Control

  struct quat qs = mkquat(setpoint->attitudeQuaternion.x, setpoint->attitudeQuaternion.y, setpoint->attitudeQuaternion.z, setpoint->attitudeQuaternion.w);

  // Calculate axis [zB_des]
  z_axis_desired = mkvec(1 - 2*fsqr(qs.y)-2*fsqr(qs.z), 2*qs.x*qs.y+2*qs.z*qs.w, 2*qs.x*qs.z -2*qs.y*qs.w);
  // [yB_des]
  y_axis_desired = mkvec(2*qs.x*qs.y -2*qs.z*qs.w, 1 - 2*fsqr(qs.x)-2*fsqr(qs.z), 2*qs.y*qs.z+2*qs.x*qs.w);
  // [xB_des]
  x_axis_desired = mkvec( 2*qs.x*qs.z+2*qs.y*qs.w ,2*qs.y*qs.z -2*qs.x*qs.w, 1 - 2*fsqr(qs.x)-2*fsqr(qs.y));

  float x = q.x;
  float y = q.y;
  float z = q.z;
  float w = q.w;
  eRa.x = (-1 + 2*fsqr(x) + 2*fsqr(y))*y_axis_desired.z + z_axis_desired.y - 2*(x*y_axis_desired.x*z + y*y_axis_desired.y*z - x*y*z_axis_desired.x + fsqr(x)*z_axis_desired.y + fsqr(z)*z_axis_desired.y - y*z*z_axis_desired.z) +    2*w*(-(y*y_axis_desired.x) - z*z_axis_desired.x + x*(y_axis_desired.y + z_axis_desired.z));
  eRa.y = x_axis_desired.z - z_axis_desired.x - 2*(fsqr(x)*x_axis_desired.z + y*(x_axis_desired.z*y - x_axis_desired.y*z) - (fsqr(y) + fsqr(z))*z_axis_desired.x + x*(-(x_axis_desired.x*z) + y*z_axis_desired.y + z*z_axis_desired.z) + w*(x*x_axis_desired.y + z*z_axis_desired.y - y*(x_axis_desired.x + z_axis_desired.z)));
  eRa.z = y_axis_desired.x - 2*(y*(x*x_axis_desired.x + y*y_axis_desired.x - x*y_axis_desired.y) + w*(x*x_axis_desired.z + y*y_axis_desired.z)) + 2*(-(x_axis_desired.z*y) + w*(x_axis_desired.x + y_axis_desired.y) + x*y_axis_desired.z)*z - 2*y_axis_desired.x*fsqr(z) + x_axis_desired.y*(-1 + 2*fsqr(x) + 2*fsqr(z));

  target_thrust.x = g_vehicleMass * setpoint->acceleration.x                       ;
  target_thrust.y = g_vehicleMass * setpoint->acceleration.y                       ;
  target_thrust.z = g_vehicleMass * (setpoint->acceleration.z + GRAVITY_MAGNITUDE) ;

  z_axis = mcolumn(mtranspose(R), 2);

  // Current thrust and torques

  current_thrust_a = vdot(target_thrust, z_axis) + vdot(mkvec(gain_setpoint.Gain1[0],gain_setpoint.Gain1[1],gain_setpoint.Gain1[2]),r_error) \
  		    + vdot(mkvec(gain_setpoint.Gain1[3],gain_setpoint.Gain1[4],gain_setpoint.Gain1[5]),v_error) \
  			+ vdot(mkvec(gain_setpoint.Gain1[6],gain_setpoint.Gain1[7],gain_setpoint.Gain1[8]),eRa) \
  			+ vdot(mkvec(gain_setpoint.Gain1[9],gain_setpoint.Gain1[10],gain_setpoint.Gain1[11]),ew); \

   #define FILLCONTROL(x,i)\
   x = vdot(mkvec(gain_setpoint.Gain ## i[0],gain_setpoint.Gain ## i[1],gain_setpoint.Gain ## i[2]),r_error) \
	 + vdot(mkvec(gain_setpoint.Gain ## i[3],gain_setpoint.Gain ## i[4],gain_setpoint.Gain ## i[5]),v_error) \
	 + vdot(mkvec(gain_setpoint.Gain ## i[6],gain_setpoint.Gain ## i[7],gain_setpoint.Gain ## i[8]),eRa) \
	 + vdot(mkvec(gain_setpoint.Gain ## i[9],gain_setpoint.Gain ## i[10],gain_setpoint.Gain ## i[11]),ew); \

   FILLCONTROL(Ma.x,2)
   FILLCONTROL(Ma.y,3)
   FILLCONTROL(Ma.z,4)
   #undef FILLCONTROL

   current_thrust_a = kf*current_thrust_a;

   Ma.x = clamp(kf_l*Ma.x, -32000, 32000);
   Ma.y = clamp(kf_l*Ma.y, -32000, 32000);
   Ma.z = clamp(km*Ma.z, -32000, 32000);


  // Mellinger Control

   if (setpoint->mode.x == modeAbs) {
    target_thrust.x = g_vehicleMass * setpoint->acceleration.x                       + kp_xy * r_error.x + kd_xy * v_error.x + ki_xy * i_error_x;
    target_thrust.y = g_vehicleMass * setpoint->acceleration.y                       + kp_xy * r_error.y + kd_xy * v_error.y + ki_xy * i_error_y;
    target_thrust.z = g_vehicleMass * (setpoint->acceleration.z + GRAVITY_MAGNITUDE) + kp_z  * r_error.z + kd_z  * v_error.z + ki_z  * i_error_z;
  } else {
    target_thrust.x = -sinf(radians(setpoint->attitude.pitch));
    target_thrust.y = -sinf(radians(setpoint->attitude.roll));
    // In case of a timeout, the commander tries to level, ie. x/y are disabled, but z will use the previous setting
    // In that case we ignore the last feedforward term for acceleration
    if (setpoint->mode.z == modeAbs) {
      target_thrust.z = g_vehicleMass * GRAVITY_MAGNITUDE + kp_z  * r_error.z + kd_z  * v_error.z + ki_z  * i_error_z;
    } else {
      target_thrust.z = 1;
    }
  }

  // Rate-controlled YAW is moving YAW angle setpoint
  if (setpoint->mode.yaw == modeVelocity) {
    desiredYaw = state->attitude.yaw + setpoint->attitudeRate.yaw * dt;
  } else if (setpoint->mode.yaw == modeAbs) {
    desiredYaw = setpoint->attitude.yaw;
  } else if (setpoint->mode.quat == modeAbs) {
    struct quat setpoint_quat = mkquat(setpoint->attitudeQuaternion.x, setpoint->attitudeQuaternion.y, setpoint->attitudeQuaternion.z, setpoint->attitudeQuaternion.w);
    struct vec rpy = quat2rpy(setpoint_quat);
    desiredYaw = degrees(rpy.z);
  }

  // Z-Axis [zB]
  z_axis = mcolumn(R, 2);

  // yaw correction (only if position control is not used)
  if (setpoint->mode.x != modeAbs) {
    struct vec x_yaw = mcolumn(R, 0);
    x_yaw.z = 0;
    x_yaw = vnormalize(x_yaw);
    struct vec y_yaw = vcross(mkvec(0, 0, 1), x_yaw);
    struct mat33 R_yaw_only = mcolumns(x_yaw, y_yaw, mkvec(0, 0, 1));
    target_thrust = mvmult(R_yaw_only, target_thrust);
  }

  // Current thrust [F]
  current_thrust = vdot(target_thrust, z_axis);

  // Calculate axis [zB_des]
  z_axis_desired = vnormalize(target_thrust);

  // [xC_des]
  // x_axis_desired = z_axis_desired x [sin(yaw), cos(yaw), 0]^T
  x_c_des.x = cosf(radians(desiredYaw));
  x_c_des.y = sinf(radians(desiredYaw));
  x_c_des.z = 0;
  // [yB_des]
  y_axis_desired = vnormalize(vcross(z_axis_desired, x_c_des));
  // [xB_des]
  x_axis_desired = vcross(y_axis_desired, z_axis_desired);

  pitch_desired = degrees(asinf(-x_axis_desired.z));
  roll_desired  = degrees(atan2f(y_axis_desired.z, z_axis_desired.z));

  // [eR]
  // Slow version
  // struct mat33 Rdes = mcolumns(
  //   mkvec(x_axis_desired.x, x_axis_desired.y, x_axis_desired.z),
  //   mkvec(y_axis_desired.x, y_axis_desired.y, y_axis_desired.z),
  //   mkvec(z_axis_desired.x, z_axis_desired.y, z_axis_desired.z));

  // struct mat33 R_transpose = mtranspose(R);
  // struct mat33 Rdes_transpose = mtranspose(Rdes);

  // struct mat33 eRM = msub(mmult(Rdes_transpose, R), mmult(R_transpose, Rdes));

  // eR.x = eRM.m[2][1];
  // eR.y = -eRM.m[0][2];
  // eR.z = eRM.m[1][0];

  // Fast version (generated using Mathematica)

  eR.x = (-1 + 2*fsqr(x) + 2*fsqr(y))*y_axis_desired.z + z_axis_desired.y - 2*(x*y_axis_desired.x*z + y*y_axis_desired.y*z - x*y*z_axis_desired.x + fsqr(x)*z_axis_desired.y + fsqr(z)*z_axis_desired.y - y*z*z_axis_desired.z) +    2*w*(-(y*y_axis_desired.x) - z*z_axis_desired.x + x*(y_axis_desired.y + z_axis_desired.z));
  eR.y = x_axis_desired.z - z_axis_desired.x - 2*(fsqr(x)*x_axis_desired.z + y*(x_axis_desired.z*y - x_axis_desired.y*z) - (fsqr(y) + fsqr(z))*z_axis_desired.x + x*(-(x_axis_desired.x*z) + y*z_axis_desired.y + z*z_axis_desired.z) + w*(x*x_axis_desired.y + z*z_axis_desired.y - y*(x_axis_desired.x + z_axis_desired.z)));
  eR.z = y_axis_desired.x - 2*(y*(x*x_axis_desired.x + y*y_axis_desired.x - x*y_axis_desired.y) + w*(x*x_axis_desired.z + y*y_axis_desired.z)) + 2*(-(x_axis_desired.z*y) + w*(x_axis_desired.x + y_axis_desired.y) + x*y_axis_desired.z)*z - 2*y_axis_desired.x*fsqr(z) + x_axis_desired.y*(-1 + 2*fsqr(x) + 2*fsqr(z));

  // Account for Crazyflie coordinate system
  eR.y = -eR.y;

  if (prev_omega_roll == prev_omega_roll) { /*d part initialized*/
    err_d_roll = ((radians(setpoint->attitudeRate.roll) - prev_setpoint_omega_roll) - (stateAttitudeRateRoll - prev_omega_roll)) / dt;
    err_d_pitch = (-(radians(setpoint->attitudeRate.pitch) - prev_setpoint_omega_pitch) - (stateAttitudeRatePitch - prev_omega_pitch)) / dt;
  }
  prev_omega_roll = stateAttitudeRateRoll;
  prev_omega_pitch = stateAttitudeRatePitch;
  prev_setpoint_omega_roll = radians(setpoint->attitudeRate.roll);
  prev_setpoint_omega_pitch = radians(setpoint->attitudeRate.pitch);

  // Integral Error
  i_error_m_x += -(eR.x) * dt;
  i_error_m_x = clamp(i_error_m_x, -i_range_m_xy, i_range_m_xy);

  i_error_m_y += -(eR.y) * dt;
  i_error_m_y = clamp(i_error_m_y, -i_range_m_xy, i_range_m_xy);

  i_error_m_z += -(eR.z) * dt;
  i_error_m_z = clamp(i_error_m_z, -i_range_m_z, i_range_m_z);

  // Moment:
  M.x = -kR_xy * eR.x + kw_xy * ew.x + ki_m_xy * i_error_m_x + kd_omega_rp * err_d_roll;
  M.y = -kR_xy * eR.y + kw_xy * ew.y + ki_m_xy * i_error_m_y + kd_omega_rp * err_d_pitch;
  M.z = -kR_z  * eR.z + kw_z  * ew.z + ki_m_z  * i_error_m_z;

  if (setpoint->mode.z == modeDisable) {
    current_thrust = setpoint->thrust;
  } else {
    current_thrust = massThrust * current_thrust;
   }

    M.x = clamp(M.x, -32000, 32000);
    M.y = clamp(M.y, -32000, 32000);
    M.z = clamp(-M.z, -32000, 32000);

    control->thrust = alpha*current_thrust_a + (1-alpha)*current_thrust;
    if (control->thrust !=0)
    {
    	control->roll = alpha*Ma.x + (1-alpha)*M.x;
    	control->pitch = alpha*Ma.y + (1-alpha)*M.y;
    	control->yaw = alpha*Ma.z + (1-alpha)*M.z;
    }
    else{
    	control->roll = 0.0;
    	control->pitch = 0.0;
    	control->yaw = 0.0;
    }


  // Output

}

PARAM_GROUP_START(ctrlMel)
PARAM_ADD(PARAM_FLOAT, kp_xy, &kp_xy)
PARAM_ADD(PARAM_FLOAT, kd_xy, &kd_xy)
PARAM_ADD(PARAM_FLOAT, ki_xy, &ki_xy)
PARAM_ADD(PARAM_FLOAT, i_range_xy, &i_range_xy)
PARAM_ADD(PARAM_FLOAT, kp_z, &kp_z)
PARAM_ADD(PARAM_FLOAT, kd_z, &kd_z)
PARAM_ADD(PARAM_FLOAT, ki_z, &ki_z)
PARAM_ADD(PARAM_FLOAT, i_range_z, &i_range_z)
PARAM_ADD(PARAM_FLOAT, mass, &g_vehicleMass)
PARAM_ADD(PARAM_FLOAT, massThrust, &massThrust)
PARAM_ADD(PARAM_FLOAT, kR_xy, &kR_xy)
PARAM_ADD(PARAM_FLOAT, kR_z, &kR_z)
PARAM_ADD(PARAM_FLOAT, kw_xy, &kw_xy)
PARAM_ADD(PARAM_FLOAT, kw_z, &kw_z)
PARAM_ADD(PARAM_FLOAT, ki_m_xy, &ki_m_xy)
PARAM_ADD(PARAM_FLOAT, ki_m_z, &ki_m_z)
PARAM_ADD(PARAM_FLOAT, kd_omega_rp, &kd_omega_rp)
PARAM_ADD(PARAM_FLOAT, i_range_m_xy, &i_range_m_xy)
PARAM_ADD(PARAM_FLOAT, i_range_m_z, &i_range_m_z)
PARAM_GROUP_STOP(ctrlMel)

LOG_GROUP_START(ctrlAgr)
LOG_ADD(LOG_FLOAT, step_time, &dt)
LOG_ADD(LOG_FLOAT, zdx, &z_axis_desired.x)
LOG_ADD(LOG_FLOAT, zdy, &z_axis_desired.y)
LOG_ADD(LOG_FLOAT, zdz, &z_axis_desired.z)
LOG_ADD(LOG_FLOAT, i_err_x, &i_error_x)
LOG_ADD(LOG_FLOAT, i_err_y, &i_error_y)
LOG_ADD(LOG_FLOAT, i_err_z, &i_error_z)
LOG_ADD(LOG_FLOAT, rollddes, &rollddes)
LOG_ADD(LOG_FLOAT, rolld, &stateAttitudeRateRoll)
LOG_ADD(LOG_FLOAT, pitchd, &stateAttitudeRatePitch)
LOG_ADD(LOG_FLOAT, gain1, &gain_setpoint.Gain1[0])
LOG_ADD(LOG_FLOAT, gain2, &gain_setpoint.Gain1[1])
LOG_ADD(LOG_FLOAT, gain3, &gain_setpoint.Gain1[2])
LOG_ADD(LOG_FLOAT, gain4, &gain_setpoint.Gain1[3])
LOG_ADD(LOG_FLOAT, mass, &g_vehicleMass)
LOG_GROUP_STOP(ctrlAgr)
