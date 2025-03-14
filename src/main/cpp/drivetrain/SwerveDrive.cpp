#include "drivetrain/SwerveDrive.h"
#include "NTUtil.h"

#include <networktables/NetworkTableInstance.h>
#include <units/math.h>

using namespace wom;

void SwerveModuleConfig::WriteNT(std::shared_ptr<nt::NetworkTable> table) const {
  std::array<double, 2> pos{ position.X().value(), position.Y().value() };
  table->GetEntry("position").SetDoubleArray(std::span(pos));
  table->GetEntry("wheelRadius").SetDouble(wheelRadius.value());
}

SwerveModule::SwerveModule(std::string path, SwerveModuleConfig config, SwerveModule::angle_pid_conf_t anglePID, SwerveModule::velocity_pid_conf_t velocityPID) 
  : _config(config),
    _anglePIDController(path + "/pid/angle", anglePID),
    _velocityPIDController(path + "/pid/velocity", velocityPID),
    _table(nt::NetworkTableInstance::GetDefault().GetTable(path))
{
  _anglePIDController.SetWrap(360_deg);
}

void SwerveModule::OnStart() {
  //_offset = offset;
  _anglePIDController.Reset();
  _velocityPIDController.Reset();
}

void SwerveModule::OnUpdate(units::second_t dt) {
  units::volt_t driveVoltage{0};
  units::volt_t turnVoltage{0};

  switch(_state) {
    case SwerveModuleState::kIdle:
      driveVoltage = 0_V;
      turnVoltage = 0_V;
      break;
    case SwerveModuleState::kPID:
      {
        auto feedforward = _config.driveMotor.motor.Voltage(0_Nm, units::radians_per_second_t{(_velocityPIDController.GetSetpoint() / _config.wheelRadius).value()});
        driveVoltage = _velocityPIDController.Calculate(GetSpeed(), dt, feedforward);
        turnVoltage = _anglePIDController.Calculate(_config.turnMotor.encoder->GetEncoderPosition(), dt);
      }
      break;
  }

  units::newton_meter_t torqueLimit = 50_kg/4 * _config.wheelRadius * _currentAccelerationLimit;
  units::volt_t voltageMax = _config.driveMotor.motor.Voltage(torqueLimit, _config.driveMotor.encoder->GetEncoderAngularVelocity());
  units::volt_t voltageMin = _config.driveMotor.motor.Voltage(-torqueLimit, _config.driveMotor.encoder->GetEncoderAngularVelocity());

  driveVoltage = units::math::max(units::math::min(driveVoltage, voltageMax), voltageMin);

  //driveVoltage = units::math::min(driveVoltage, 10_V);
  turnVoltage = units::math::min(turnVoltage, 7_V);

  driveVoltage = units::math::min(units::math::max(driveVoltage, -_driveModuleVoltageLimit), _driveModuleVoltageLimit); // was originally 10_V
  // std::cout << "drive-voltage: " << driveVoltage.value() << std::endl;
  units::volt_t turnVoltageMax = 7_V - (driveVoltage * (7_V / 10_V));
  turnVoltage = units::math::min(units::math::max(turnVoltage, -turnVoltageMax), turnVoltageMax);
  // turnVoltage = units::math::min(units::math::max(turnVoltage, -7_V), 7_V);
  // std::cout << "turn-voltage-max: " << turnVoltageMax.value() << std::endl;

  _config.driveMotor.transmission->SetVoltage(driveVoltage);
  _config.turnMotor.transmission->SetVoltage(turnVoltage);

  _table->GetEntry("speed").SetDouble(GetSpeed().value());
  _table->GetEntry("angle").SetDouble(_config.turnMotor.encoder->GetEncoderPosition().convert<units::degree>().value());
  _config.WriteNT(_table->GetSubTable("config"));
}

// double SwerveModule::GetCancoderPosition() {
//   return (_config.turnMotor.encoder->GetEncoderPosition().value());
// }

void SwerveModule::SetAccelerationLimit(units::meters_per_second_squared_t limit){
  _currentAccelerationLimit = limit;
}

void SwerveDrive::SetAccelerationLimit(units::meters_per_second_squared_t limit){
  for (int motorNumber = 0; motorNumber < 4; motorNumber++){
    _modules[motorNumber].SetAccelerationLimit(limit);
  }
}

void SwerveModule::SetVoltageLimit(units::volt_t driveModuleVoltageLimit ) {
  _driveModuleVoltageLimit = driveModuleVoltageLimit;
}

void SwerveModule::SetIdle() {
  _state = SwerveModuleState::kIdle;
}

void SwerveModule::SetZero(units::second_t dt) {
  SetPID(0_rad, 0_mps, dt);
  _state = SwerveModuleState::kPID;
}

void SwerveModule::SetPID(units::radian_t angle, units::meters_per_second_t speed, units::second_t dt) {
  _state = SwerveModuleState::kPID;
  // @liam start added
  double diff = std::fmod((_anglePIDController.GetSetpoint() - angle).convert<units::degree>().value(), 360);
  if (std::abs(diff) >= 90) {
    speed *= -1;
    angle += 180_deg;
  }
  // @liam end added

  _anglePIDController.SetSetpoint(angle);
  _velocityPIDController.SetSetpoint(speed);
}

void SwerveModule::ModuleVectorHandler(frc::ChassisSpeeds speeds){
  units::meters_per_second_t xVelocityComponent = 1_mps * (speeds.vx.value() + speeds.omega.value() * _config.position.X().value());
  units::meters_per_second_t yVelocityComponent = 1_mps * (speeds.vy.value() + speeds.omega.value() * _config.position.Y().value());

  units::meters_per_second_t velocity = 1_mps * sqrt(pow(xVelocityComponent.value(), 2) + pow(yVelocityComponent.value(), 2));
  units::degree_t angle = 1_rad * atan2(yVelocityComponent.value(), xVelocityComponent.value());

  _anglePIDController.SetSetpoint(angle);
  _velocityPIDController.SetSetpoint(velocity);
}

units::meters_per_second_t SwerveModule::GetSpeed() const {
  return units::meters_per_second_t{_config.driveMotor.encoder->GetEncoderAngularVelocity().value() * _config.wheelRadius.value()};
}

units::meter_t SwerveModule::GetDistance() const {
  return units::meter_t{ _config.driveMotor.encoder->GetEncoderPosition().value() * _config.wheelRadius.value() };
}

frc::SwerveModulePosition SwerveModule::GetPosition() const {
  return frc::SwerveModulePosition {
    GetDistance(),
    _config.turnMotor.encoder->GetEncoderPosition()
  };
}

const SwerveModuleConfig &SwerveModule::GetConfig() const {
  return _config;
}

void SwerveDriveConfig::WriteNT(std::shared_ptr<nt::NetworkTable> table) {
  table->GetEntry("mass").SetDouble(mass.value());
}

SwerveDrive::SwerveDrive(SwerveDriveConfig config, frc::Pose2d initialPose) :
  _config(config),
  _kinematics( _config.modules[0].position, _config.modules[1].position, _config.modules[2].position, _config.modules[3].position),
  _poseEstimator(
    _kinematics, frc::Rotation2d(0_deg),
    wpi::array<frc::SwerveModulePosition, 4> { 
      frc::SwerveModulePosition { 0_m, frc::Rotation2d{0_deg} },
      frc::SwerveModulePosition { 0_m, frc::Rotation2d{0_deg} },
      frc::SwerveModulePosition { 0_m, frc::Rotation2d{0_deg} },
      frc::SwerveModulePosition { 0_m, frc::Rotation2d{0_deg} }
    },
    initialPose,
    _config.stateStdDevs, _config.visionMeasurementStdDevs
  ),
  _anglePIDController(config.path + "/pid/heading", _config.poseAnglePID),
  _xPIDController(config.path + "/pid/x", _config.posePositionPID),
  _yPIDController(config.path + "/pid/y", _config.posePositionPID),
  _table(nt::NetworkTableInstance::GetDefault().GetTable(_config.path))
{

  _anglePIDController.SetWrap(360_deg);
  
  int i = 1;
  for (auto cfg : _config.modules) {
    _modules.emplace_back(config.path + "/modules/" + std::to_string(i), cfg, config.anglePID, config.velocityPID);
    i++;
  }

  ResetPose(initialPose);
}


frc::ChassisSpeeds FieldRelativeSpeeds::ToChassisSpeeds(const units::radian_t robotHeading) {
  return frc::ChassisSpeeds::FromFieldRelativeSpeeds(vx, vy, omega, frc::Rotation2d{robotHeading});
}

void SwerveDrive::OnUpdate(units::second_t dt) {
  switch (_state) {
    case SwerveDriveState::kZeroing:
      for (auto mod = _modules.begin(); mod < _modules.end(); mod++) {
        mod->SetZero(dt);
      }
      break;
    case SwerveDriveState::kIdle:
      for (auto mod = _modules.begin(); mod < _modules.end(); mod++) {
        mod->SetIdle();
      }
      break;
    case SwerveDriveState::kPose:
      {
        _target_fr_speeds.vx = _xPIDController.Calculate(GetPose().X(), dt);
        _target_fr_speeds.vy = _yPIDController.Calculate(GetPose().Y(), dt);
        _target_fr_speeds.omega = _anglePIDController.Calculate(GetPose().Rotation().Radians(), dt);
      }
      [[fallthrough]];
    case SwerveDriveState::kFieldRelativeVelocity:
      _target_speed = _target_fr_speeds.ToChassisSpeeds(GetPose().Rotation().Radians());
      if (isRotateToMatchJoystick){
        _target_speed.omega = _anglePIDController.Calculate(GetPose().Rotation().Radians(), dt);
      }
      // std::cout << "vx = " << _target_speed.vx.value() << " vy = " << _target_fr_speeds.vy.value() << std::endl;
      [[fallthrough]];
    case SwerveDriveState::kVelocity:
      {
        auto target_states = _kinematics.ToSwerveModuleStates(_target_speed);
        for (size_t i = 0; i < _modules.size(); i++) {
          _modules[i].SetPID(target_states[i].angle.Radians(), target_states[i].speed, dt);
          // std::cout << "module " << i << ": " << target_states[i].angle.Radians().value() << std::endl;
        }
      }
      break;
    case SwerveDriveState::kIndividualTuning: 
      _modules[_mod].SetPID(_angle, _speed, dt);
      break;

    case SwerveDriveState::kTuning:
      for (size_t i = 0; i < _modules.size(); i++) {
        _modules[i].SetPID(_angle, _speed, dt);
      }
      break;
    case SwerveDriveState::kXWheels:
      _modules[0].SetPID(45_deg, 0_mps, dt);
      _modules[1].SetPID(135_deg, 0_mps, dt);
      _modules[2].SetPID(315_deg, 0_mps, dt);
      _modules[3].SetPID(405_deg, 0_mps, dt);
      break;
    case SwerveDriveState::kFRVelocityRotationLock:
      _target_speed.vx = _xPIDController.Calculate(GetPose().X(), dt);
      _target_speed.vy = _yPIDController.Calculate(GetPose().Y(), dt);
      _target_speed.omega = _anglePIDController.Calculate(GetPose().Rotation().Radians(), dt);
      _target_speed = _requestedSpeeds.ToChassisSpeeds(GetPose().Rotation().Radians());
      auto target_states = _kinematics.ToSwerveModuleStates(_target_speed);
      for (size_t i = 0; i < _modules.size(); i++) {
        std::cout << "Speeds :" << target_states[i].speed.value() << std::endl;
        _modules[i].SetPID(target_states[i].angle.Radians(), target_states[i].speed, dt);
      }
      break;
  }

  for (auto mod = _modules.begin(); mod < _modules.end(); mod++) {
    mod->OnUpdate(dt);
  }

  _poseEstimator.Update(
    _config.gyro->GetRotation2d(),
    wpi::array<frc::SwerveModulePosition, 4>{
      _modules[0].GetPosition(),
      _modules[1].GetPosition(),
      _modules[2].GetPosition(),
      _modules[3].GetPosition()
    }
  );

  WritePose2NT(_table->GetSubTable("estimatedPose"), _poseEstimator.GetEstimatedPosition());
  _config.WriteNT(_table->GetSubTable("config"));
}

void SwerveDrive::SetXWheelState(){
  _state = SwerveDriveState::kXWheels;
}

void SwerveDrive::SetZeroing() {
  _state = SwerveDriveState::kZeroing;
}

void SwerveDrive::SetVoltageLimit(units::volt_t driveVoltageLimit) {
  for(auto mod : _modules) {
    mod.SetVoltageLimit(driveVoltageLimit);
  }
}

// double SwerveDrive::GetModuleCANPosition(int mod) {
//   return _modules[mod].GetCancoderPosition();
// }


void SwerveDrive::OnStart() {
  _xPIDController.Reset();
  _yPIDController.Reset();
  _anglePIDController.Reset();

  _modules[0].OnStart(); // front left
  _modules[1].OnStart(); // front right
  _modules[2].OnStart(); // back right 
  _modules[3].OnStart(); // back left
}

void SwerveDrive::RotateMatchJoystick(units::radian_t joystickAngle, FieldRelativeSpeeds speeds) {
  // _state = SwerveDriveState::kFRVelocityRotationLock;
  // _anglePIDController.SetSetpoint(joystickAngle);
  // _target_fr_speeds = speeds;
  _state = SwerveDriveState::kFieldRelativeVelocity;
  isRotateToMatchJoystick = true;
  _anglePIDController.SetSetpoint(joystickAngle);
  _target_fr_speeds = speeds;
}

void SwerveDrive::SetIdle() {
  _state = SwerveDriveState::kIdle;
}

void SwerveDrive::SetVelocity(frc::ChassisSpeeds speeds) {
  _state = SwerveDriveState::kVelocity;
  _target_speed = speeds;
}

void SwerveDrive::SetIsFieldRelative(bool value){
  _isFieldRelative = value;
}
bool SwerveDrive::GetIsFieldRelative(){
  return _isFieldRelative;
}

void SwerveDrive::SetIndividualTuning(int mod, units::radian_t angle, units::meters_per_second_t speed) {
  _mod = mod;
  _angle = angle;
  _speed = speed;
  _state = SwerveDriveState::kIndividualTuning;
}

void SwerveDrive::SetTuning(units::radian_t angle, units::meters_per_second_t speed) {
  _angle = angle;
  _speed = speed;
  _state = SwerveDriveState::kTuning;
}

void SwerveDrive::SetFieldRelativeVelocity(FieldRelativeSpeeds speeds) {
  _state = SwerveDriveState::kFieldRelativeVelocity;
  _target_fr_speeds = speeds;
}

void SwerveDrive::SetPose(frc::Pose2d pose) {
  _state = SwerveDriveState::kPose;
  _anglePIDController.SetSetpoint(pose.Rotation().Radians());
  _xPIDController.SetSetpoint(pose.X());
  _yPIDController.SetSetpoint(pose.Y());
}

bool SwerveDrive::IsAtSetPose() {
  return _anglePIDController.IsStable() && _xPIDController.IsStable() && _yPIDController.IsStable(0.05_m);
}

void SwerveDrive::ResetPose(frc::Pose2d pose) {
  _poseEstimator.ResetPosition(_config.gyro->GetRotation2d(),
    wpi::array<frc::SwerveModulePosition, 4>{
      _modules[0].GetPosition(),
      _modules[1].GetPosition(),
      _modules[2].GetPosition(),
      _modules[3].GetPosition()
    }, pose
  );
}

frc::Pose2d SwerveDrive::GetPose() {
  return _poseEstimator.GetEstimatedPosition();
}

void SwerveDrive::AddVisionMeasurement(frc::Pose2d pose, units::second_t timestamp) {
  _poseEstimator.AddVisionMeasurement(pose, timestamp);
}

void SwerveDrive::SetZero() {
  _state = SwerveDriveState::kZeroing;
}

/* SIMULATION */

wom::sim::SwerveDriveSim::SwerveDriveSim(SwerveDriveConfig config, units::kilogram_square_meter_t moduleJ)
  : config(config), kinematics(config.modules[0].position, config.modules[1].position, config.modules[2].position, config.modules[3].position),
    moduleJ(moduleJ),
    table(nt::NetworkTableInstance::GetDefault().GetTable(config.path + "/sim")),
    gyro(config.gyro->MakeSimGyro())
  {
    for (size_t i = 0; i < config.modules.size(); i++) {
      driveEncoders.push_back(config.modules[i].driveMotor.encoder->MakeSimEncoder());
      turnEncoders.push_back(config.modules[i].turnMotor.encoder->MakeSimEncoder());
    }
  }

void wom::sim::SwerveDriveSim::Update(units::second_t dt) {

  Eigen::Vector2d resultantForceVector{0, 0};

  totalCurrent = 0_A;

  for (size_t i = 0; i < config.modules.size(); i++) {
    /* Calculate drive motor forces */
    driveCurrents[i] = config.modules[i].driveMotor.motor.Current(
      driveSpeeds[i],
      config.modules[i].driveMotor.transmission->GetEstimatedRealVoltage()
    );
    auto drive_torque = config.modules[i].driveMotor.motor.Torque(driveCurrents[i]);
    units::newton_t force_magnitude = drive_torque / config.modules[i].wheelRadius;
    totalCurrent += driveCurrents[i];

    driveVelocity[i] += force_magnitude / (config.mass / 4) * dt;
    driveSpeeds[i] = 1_rad * driveVelocity[i] / config.modules[i].wheelRadius;
    driveEncoderAngles[i] += driveSpeeds[i] * dt;
    driveEncoders[i]->SetEncoderTurnVelocity(driveSpeeds[i]);
    driveEncoders[i]->SetEncoderTurns(driveEncoderAngles[i]);

    /* Reconcile turning motor - assuming no losses or wheel slip */
    turnCurrents[i] = config.modules[i].turnMotor.motor.Current(
      turnSpeeds[i],
      config.modules[i].turnMotor.transmission->GetEstimatedRealVoltage()
    );
    totalCurrent += turnCurrents[i];

    auto turn_torque = config.modules[i].turnMotor.motor.Torque(turnCurrents[i]);
    turnSpeeds[i] += 1_rad * turn_torque / moduleJ * dt;
    turnAngles[i] += turnSpeeds[i] * dt;
    turnEncoders[i]->SetEncoderTurnVelocity(turnSpeeds[i]);
    turnEncoders[i]->SetEncoderTurns(turnAngles[i]);

    auto mtable = table->GetSubTable("modules/" + std::to_string(i));
    mtable->GetEntry("turnTorque").SetDouble(turn_torque.value());
  }

  auto chassis_state = kinematics.ToChassisSpeeds(
    frc::SwerveModuleState { driveVelocity[0], frc::Rotation2d{turnAngles[0]} },
    frc::SwerveModuleState { driveVelocity[1], frc::Rotation2d{turnAngles[1]} },
    frc::SwerveModuleState { driveVelocity[2], frc::Rotation2d{turnAngles[2]} },
    frc::SwerveModuleState { driveVelocity[3], frc::Rotation2d{turnAngles[3]} }
  );

  /* Get body angular velocity and angle */
  angularVelocity = chassis_state.omega;
  angle += angularVelocity * dt;

  vx = chassis_state.vx;
  vy = chassis_state.vy;

  x += (vx * units::math::cos(angle) - vy * units::math::sin(angle)) * dt;
  y += (vx * units::math::sin(angle) + vy * units::math::cos(angle)) * dt;

  // Note vx, vy are in robot frame whilst x, y are in world frame
  table->GetEntry("vx").SetDouble(vx.value());
  table->GetEntry("vy").SetDouble(vy.value());
  table->GetEntry("angle").SetDouble(angle.convert<units::degree>().value());
  table->GetEntry("angularVelocity").SetDouble(angularVelocity.convert<units::degrees_per_second>().value());
  table->GetEntry("x").SetDouble(x.value());
  table->GetEntry("y").SetDouble(y.value());
  table->GetEntry("totalCurrent").SetDouble(totalCurrent.value());

  gyro->SetAngle(-angle);
}