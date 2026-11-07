// src/core/gyro.cpp for the browser: the device's own motion sensor (a phone or tablet held as the controller, the
// page's devicemotion readings in the gamepad record's ring) turns the view like a mouse, beside the right stick. The
// drift, smoothing and axes are native's; the readings are taken when the game takes the turn. No browser pad
// reports a button the game's layout leaves free, so the gyro button and recenter settings do nothing.
#include "core/gyro.h"
#include "browser/input-core.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace gyro {

namespace {

namespace ic = browser::input_core;

constexpr float kSecondsPerUs = 1e-6f;
constexpr float kMaxSampleGap = 0.05f;  // seconds; a longer gap between readings is a stall, not motion
constexpr auto kStale = std::chrono::milliseconds(100);  // turn gathered while the look update did not run is dropped
constexpr float kGravitySeconds = 0.25f;  // the accelerometer's smoothing into the controller's up
// a controller at rest stays within these through a whole window; the gyro's mean there is its drift.
// A slow steady turn passes the spread tests too, so only a mean this small is taken for drift.
constexpr float kRestSeconds = 2.0f;
constexpr float kRestGyroSpread = 1.0f;  // degrees a second
constexpr float kRestAccelSpread = 0.1f;  // metres a second squared
constexpr float kMaxDrift = 1.5f;  // degrees a second
// turns slower than this are smoothed over kSmoothSeconds, twice as fast pass as they are
constexpr float kSmoothSpeed = 5.0f;  // degrees a second
constexpr float kSmoothSeconds = 0.1f;

// the controller's axes: x right, y up, z toward the player
struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
float Largest(const Vec3& a) { return std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}); }

// the lowest and highest reading on each axis
struct Spread {
  Vec3 low;
  Vec3 high;
  bool empty = true;

  void Add(const Vec3& v) {
    if (empty) {
      low = v;
      high = v;
      empty = false;
      return;
    }
    low = {std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
    high = {std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
  }
  float Widest() const { return empty ? 0.0f : Largest(Sub(high, low)); }
};

// the gyro's mean over a window
struct Average {
  Vec3 sum;
  int count = 0;
  float seconds = 0;

  void Push(const Vec3& v, float dt) {
    sum = Add(sum, v);
    ++count;
    seconds += dt;
  }
  Vec3 Mean() const { return count ? Scale(sum, 1.0f / float(count)) : Vec3{}; }
};

// the window the controller is tested for rest through
struct RestWindow {
  Spread gyro;
  Spread accel;
  Average mean;
};

struct Motion {
  bool read = false;  // a reading arrived
  uint32_t tail = 0;  // the ring's next reading
  uint32_t gyro_us = 0;  // the clock of the last readings
  uint32_t accel_us = 0;
  bool gyro_seen = false;
  bool accel_seen = false;
  Vec3 gravity;  // smoothed accelerometer, pointing up
  Vec3 drift;  // degrees a second the gyro reads at rest
  Vec3 smoothed;  // the slow share of the turn, degrees a second
  RestWindow rest;
  bool calibrating = false;  // the next still window sets the drift, however large
  bool calibrated = false;  // by hand: rest windows stop, a slow steady turn would pass for drift
  // degrees turned since the last take: about the controller's up, its front, the world's up; pitch down positive
  float yaw = 0;
  float roll = 0;
  float world_yaw = 0;
  float pitch = 0;
};

std::mutex g_lock;
Motion g_motion;

// seconds since the last reading, 0 for the first and for a stall
float Gap(uint32_t& last, bool& seen, uint32_t now) {
  const int32_t us = seen ? int32_t(now - last) : 0;
  last = now;
  seen = true;
  const float seconds = float(us) * kSecondsPerUs;
  return seconds > 0 && seconds <= kMaxSampleGap ? seconds : 0.0f;
}

Vec3 Up(const Motion& m) {
  const float length = Length(m.gravity);
  if (length < 1.0f) { return {0, 1, 0}; }  // no accelerometer yet
  return Scale(m.gravity, 1.0f / length);
}

void ReadAccel(Motion& m, const Vec3& accel, uint32_t us) {
  const bool first = !m.accel_seen;
  const float dt = Gap(m.accel_us, m.accel_seen, us);
  const float k = first ? 1.0f : dt / (kGravitySeconds + dt);
  m.gravity = Add(m.gravity, Scale(Sub(accel, m.gravity), k));
  m.rest.accel.Add(accel);
}

// a window the controller lay still through sets the drift to the gyro's mean over it
void UpdateDrift(Motion& m, const Vec3& gyro, float dt) {
  RestWindow& w = m.rest;
  w.gyro.Add(gyro);
  w.mean.Push(gyro, dt);
  if (w.mean.seconds < kRestSeconds) { return; }
  const Vec3 mean = w.mean.Mean();
  const bool still = w.gyro.Widest() < kRestGyroSpread && w.accel.Widest() < kRestAccelSpread;
  if (still && m.calibrating) {
    m.drift = mean;
    m.calibrating = false;
    m.calibrated = true;
  } else if (still && !m.calibrated && Largest(mean) < kMaxDrift) {
    m.drift = mean;
  }
  w = {};
}

// the slow share of a turn is smoothed, the fast share passes as it is: sensor noise goes, quick aim keeps no lag
Vec3 ApplySmoothing(Motion& m, const Vec3& turn, float dt) {
  const float fast = std::clamp(Length(turn) / kSmoothSpeed - 1.0f, 0.0f, 1.0f);
  const float k = dt / (kSmoothSeconds + dt);
  m.smoothed = Add(m.smoothed, Scale(Sub(Scale(turn, 1.0f - fast), m.smoothed), k));
  return Add(Scale(turn, fast), m.smoothed);
}

void ReadGyro(Motion& m, const Vec3& gyro, uint32_t us) {
  const float dt = Gap(m.gyro_us, m.gyro_seen, us);
  UpdateDrift(m, gyro, dt);
  if (m.calibrating) { return; }
  const Vec3 turn = ApplySmoothing(m, Sub(gyro, m.drift), dt);
  m.yaw += turn.y * dt;
  m.roll += turn.z * dt;  // the left side down turns left
  m.world_yaw += Dot(turn, Up(m)) * dt;
  m.pitch -= turn.x * dt;  // turning about x raises the controller's front
}

Vec3 ReadVector(const ic::GamepadWords& words, uint32_t at) {
  return {std::bit_cast<float>(words[at].load(std::memory_order_relaxed)), std::bit_cast<float>(words[at + 1].load(std::memory_order_relaxed)),
          std::bit_cast<float>(words[at + 2].load(std::memory_order_relaxed))};
}

// the readings the page wrote since the last take; a ring that ran over keeps its newest
void Drain(Motion& m, const ic::GamepadWords& words) {
  const uint32_t head = words[ic::kMotionHead].load(std::memory_order_acquire);
  if (head - m.tail > ic::kMotionEntries) { m.tail = head - ic::kMotionEntries; }
  for (; m.tail != head; ++m.tail) {
    const uint32_t entry = ic::kMotionRing + m.tail % ic::kMotionEntries * ic::kMotionEntryWords;
    const uint32_t us = words[entry].load(std::memory_order_relaxed);
    m.read = true;
    if (words[entry + 7].load(std::memory_order_relaxed)) { ReadAccel(m, ReadVector(words, entry + 4), us); }
    ReadGyro(m, ReadVector(words, entry + 1), us);
  }
}

float AxisTurn(const Motion& m, uint32_t axis) {
  if (axis == kRoll) { return m.roll; }
  if (axis == kWorld) { return m.world_yaw; }
  return m.yaw;
}

}  // namespace

Turn Take(const Settings& s) {
  static std::chrono::steady_clock::time_point last_take{};
  std::lock_guard lock(g_lock);
  Motion& m = g_motion;
  const ic::GamepadWords* words = ic::Gamepad();
  if (!words) { return {}; }
  if (!s.enabled) {
    m.tail = (*words)[ic::kMotionHead].load(std::memory_order_acquire);
    return {};
  }
  const auto now = std::chrono::steady_clock::now();
  const bool stale = now - last_take > kStale;
  last_take = now;
  Drain(m, *words);
  Turn turn;
  if (!stale) {
    turn.yaw = AxisTurn(m, s.axis) * s.sensitivity;
    turn.pitch = m.pitch * s.sensitivity;
  }
  m.yaw = 0;
  m.roll = 0;
  m.world_yaw = 0;
  m.pitch = 0;
  return turn;
}

void StartCalibration() {
  std::lock_guard lock(g_lock);
  if (!g_motion.read) { return; }  // no gyro read yet
  g_motion.calibrating = true;
  g_motion.rest = {};
}

bool Calibrating() {
  std::lock_guard lock(g_lock);
  return g_motion.calibrating;
}

}  // namespace gyro
