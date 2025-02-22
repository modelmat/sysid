// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#pragma once

#include <vector>

#include <units/time.h>

namespace sysid {

/**
 * Represents each data point after it is cleaned and various parameters are
 * calculated.
 */
struct PreparedData {
  /**
   * The timestamp of the data point.
   */
  units::second_t timestamp;

  /**
   * The voltage of the data point.
   */
  double voltage;

  /**
   * The position of the data point.
   */
  double position;

  /**
   * The velocity of the data point.
   */
  double velocity;

  /**
   * The velocity of the next data point.
   */
  double nextVelocity = 0.0;

  /**
   * The difference in timestamps between this point and the next point.
   */
  units::second_t dt = 0_s;

  /**
   * The acceleration of the data point
   */
  double acceleration = 0.0;

  /**
   * The cosine value of the data point. This is only used for arm data where we
   * take the cosine of the position.
   */
  double cos = 0.0;

  /**
   * Equality operator between PreparedData structs
   *
   * @param rhs Another PreparedData struct
   * @return If the other struct is equal to this one
   */
  constexpr bool operator==(const PreparedData& rhs) const {
    return timestamp == rhs.timestamp && voltage == rhs.voltage &&
           position == rhs.position && velocity == rhs.velocity &&
           nextVelocity == rhs.nextVelocity && dt == rhs.dt &&
           acceleration == rhs.acceleration && cos == rhs.cos;
  }
};

/**
 * Storage used by the analysis manger.
 */
struct Storage {
  /**
   * Dataset for slow (aka quasistatic) test
   */
  std::vector<PreparedData> slow;

  /**
   * Dataset for fast (aka dynamic) test
   */
  std::vector<PreparedData> fast;
};

}  // namespace sysid
