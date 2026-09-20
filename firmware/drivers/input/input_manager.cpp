#include "drivers/input_manager.h"

#include <Arduino.h>

#include "app/app_config.h"
#include "board/board_crowpanel_579.h"

namespace drivers {

bool InputManager::begin() {
  pinMode(board::kBtnMenu, INPUT_PULLUP);
  pinMode(board::kBtnExit, INPUT_PULLUP);
  pinMode(board::kEncoderSw, INPUT_PULLUP);
  pinMode(board::kEncoderA, INPUT_PULLUP);
  pinMode(board::kEncoderB, INPUT_PULLUP);

  const bool menu = digitalRead(board::kBtnMenu) != LOW;
  const bool exit_pin = digitalRead(board::kBtnExit) != LOW;
  const bool sw = digitalRead(board::kEncoderSw) != LOW;
  const bool a = digitalRead(board::kEncoderA) != LOW;
  const bool b = digitalRead(board::kEncoderB) != LOW;

  menu_.begin(board::kInputActiveLow, menu, app::kButtonDebounceMs,
              app::kLongPressMs);
  exit_.begin(board::kInputActiveLow, exit_pin, app::kButtonDebounceMs,
              app::kLongPressMs);
  switch_.begin(board::kInputActiveLow, sw, app::kButtonDebounceMs,
                app::kLongPressMs);

  encoder_a_.begin(a, app::kEncoderDebounceMs);
  encoder_b_.begin(b, app::kEncoderDebounceMs);
  pulse_.begin(a, b);
  coalescer_.begin(app::kRotaryQuietMs, app::kRotaryMaxPending);

  // Seed the diagnostics from the levels we just sampled. anyHeld() is the
  // guard against the sleep/re-wake trap, and it must not claim "nothing is
  // held" during the window between begin() and the first poll().
  diag_ = hal::InputDiagnostics();
  diag_.menu_level = menu;
  diag_.exit_level = exit_pin;
  diag_.switch_level = sw;
  diag_.encoder_a_level = a;
  diag_.encoder_b_level = b;

  // Touch is additive: a panel that does not answer leaves the front panel
  // working exactly as before, so begin() never fails on its account.
  tap_.begin(app::kTouchLongPressMs, app::kTouchSlopPx);
  touch_down_ = false;
  touch_frame_ms_ = millis();
  // Keep whatever orientation was set before this begin() -- the saved one is
  // loaded off the card at boot, and begin() runs again on every wake.
  const uint8_t orientation = started_ ? touchOrientation()
                                       : static_cast<uint8_t>(
                                             (board::kTouchSwapXY ? 1 : 0) |
                                             (board::kTouchInvertX ? 2 : 0) |
                                             (board::kTouchInvertY ? 4 : 0));
  mapping_ = util::TouchMapping();
  if (touch_ != nullptr && touch_->begin()) {
    const hal::TouchInfo info = touch_->info();
    mapping_.raw_width = info.raw_width;
    mapping_.raw_height = info.raw_height;
    mapping_.out_width = board::kWidth;
    mapping_.out_height = board::kHeight;
    setTouchOrientation(orientation);
    diag_.touch_present = true;
  }

  head_ = 0;
  count_ = 0;
  last_activity_ms_ = millis();
  started_ = true;
  return true;
}

void InputManager::push(hal::InputSource source, hal::InputAction action,
                        int16_t delta, uint32_t now_ms) {
  push(source, action, delta, 0, 0, now_ms);
}

void InputManager::push(hal::InputSource source, hal::InputAction action,
                        int16_t delta, int16_t x, int16_t y, uint32_t now_ms) {
  last_activity_ms_ = now_ms;
  if (count_ >= kQueueSize) {
    // Drop the oldest rather than the newest: the most recent intent wins.
    head_ = static_cast<uint8_t>((head_ + 1) % kQueueSize);
    --count_;
  }
  const uint8_t tail = static_cast<uint8_t>((head_ + count_) % kQueueSize);
  queue_[tail].source = source;
  queue_[tail].action = action;
  queue_[tail].delta = delta;
  queue_[tail].x = x;
  queue_[tail].y = y;
  queue_[tail].timestamp_ms = now_ms;
  ++count_;
}

bool InputManager::nextEvent(hal::InputEvent* out) {
  if (count_ == 0) return false;
  if (out != nullptr) *out = queue_[head_];
  head_ = static_cast<uint8_t>((head_ + 1) % kQueueSize);
  --count_;
  return true;
}

void InputManager::pumpButton(util::Button& button, hal::InputSource source,
                              bool raw, uint32_t now_ms,
                              uint32_t* click_counter) {
  switch (button.update(raw, now_ms)) {
    case util::ButtonEvent::kDown:
      push(source, hal::InputAction::kDown, 0, now_ms);
      break;
    case util::ButtonEvent::kClick:
      if (click_counter != nullptr) ++(*click_counter);
      push(source, hal::InputAction::kClick, 0, now_ms);
      break;
    case util::ButtonEvent::kLongPress:
      ++diag_.long_press_count;
      push(source, hal::InputAction::kLongPress, 0, now_ms);
      break;
    case util::ButtonEvent::kUp:
      push(source, hal::InputAction::kUp, 0, now_ms);
      break;
    case util::ButtonEvent::kNone:
    default:
      break;
  }
}

void InputManager::setTouchOrientation(uint8_t packed) {
  mapping_.swap_xy = (packed & 1) != 0;
  mapping_.invert_x = (packed & 2) != 0;
  mapping_.invert_y = (packed & 4) != 0;
}

uint8_t InputManager::touchOrientation() const {
  return static_cast<uint8_t>((mapping_.swap_xy ? 1 : 0) |
                              (mapping_.invert_x ? 2 : 0) |
                              (mapping_.invert_y ? 4 : 0));
}

// One finger's worth of meaning out of whatever the panel reports. The GT911
// sends a frame only when something changes, so "no frame" means the finger
// (or the absence of one) is unchanged since the last poll -- which is why
// the last position is remembered rather than re-read.
void InputManager::pumpTouch(uint32_t now_ms) {
  if (touch_ == nullptr || !diag_.touch_present) return;

  hal::TouchFrame frame;
  if (touch_->poll(&frame)) {
    touch_frame_ms_ = now_ms;
    touch_down_ = frame.count > 0;
    diag_.touch_fingers = frame.count;
    if (touch_down_) {
      // The first point only: a palm resting on the glass must not act.
      const util::TouchXY xy =
          util::MapTouchPoint(frame.points[0].x, frame.points[0].y, mapping_);
      touch_x_ = xy.x;
      touch_y_ = xy.y;
      diag_.touch_raw_x = frame.points[0].x;
      diag_.touch_raw_y = frame.points[0].y;
      diag_.touch_x = xy.x;
      diag_.touch_y = xy.y;
    }
  } else if (touch_down_ &&
             static_cast<uint32_t>(now_ms - touch_frame_ms_) >=
                 app::kTouchStaleMs) {
    // The bus went quiet mid-gesture (see app::kTouchStaleMs). Let the finger
    // go: a lift the UI ignores is recoverable, a finger that is never lifted
    // keeps the device awake until the battery is gone.
    touch_down_ = false;
    diag_.touch_fingers = 0;
  }

  const util::TouchEvent event =
      tap_.update(touch_down_, touch_x_, touch_y_, now_ms);
  diag_.touch_down = tap_.down();
  switch (event) {
    case util::TouchEvent::kDown:
      push(hal::InputSource::kTouch, hal::InputAction::kDown, 0, tap_.x(),
           tap_.y(), now_ms);
      break;
    case util::TouchEvent::kTap:
      ++diag_.touch_tap_count;
      push(hal::InputSource::kTouch, hal::InputAction::kClick, 0, tap_.x(),
           tap_.y(), now_ms);
      break;
    case util::TouchEvent::kLongPress:
      ++diag_.long_press_count;
      push(hal::InputSource::kTouch, hal::InputAction::kLongPress, 0, tap_.x(),
           tap_.y(), now_ms);
      break;
    case util::TouchEvent::kUp:
      push(hal::InputSource::kTouch, hal::InputAction::kUp, 0, tap_.lastX(),
           tap_.lastY(), now_ms);
      break;
    case util::TouchEvent::kNone:
    default:
      // A finger sliding is not an event, but it is activity: without this
      // the idle timer could sleep the device under a moving finger.
      if (tap_.down()) last_activity_ms_ = now_ms;
      break;
  }
}

void InputManager::poll(uint32_t now_ms) {
  if (!started_) return;

  const bool menu = digitalRead(board::kBtnMenu) != LOW;
  const bool exit_pin = digitalRead(board::kBtnExit) != LOW;
  const bool sw = digitalRead(board::kEncoderSw) != LOW;
  const bool raw_a = digitalRead(board::kEncoderA) != LOW;
  const bool raw_b = digitalRead(board::kEncoderB) != LOW;

  diag_.menu_level = menu;
  diag_.exit_level = exit_pin;
  diag_.switch_level = sw;
  diag_.encoder_a_level = raw_a;
  diag_.encoder_b_level = raw_b;

  pumpButton(menu_, hal::InputSource::kMenu, menu, now_ms, &diag_.menu_count);
  pumpButton(exit_, hal::InputSource::kExit, exit_pin, now_ms,
             &diag_.exit_count);
  pumpButton(switch_, hal::InputSource::kEncoderSwitch, sw, now_ms,
             &diag_.switch_count);

  const bool a_changed = encoder_a_.update(raw_a, now_ms);
  const bool b_changed = encoder_b_.update(raw_b, now_ms);
  const bool a = encoder_a_.level();
  const bool b = encoder_b_.level();
  if (a_changed || b_changed) {
    diag_.edge_count++;
    last_activity_ms_ = now_ms;
  }

  const int8_t step = pulse_.update(a, b);
  diag_.pulse_position = pulse_.position();
  coalescer_.add(step, now_ms);

  pumpTouch(now_ms);

  // Spec section 14: release accumulated detents as one event so a fast spin
  // costs one refresh, not one per detent.
  if (coalescer_.ready(now_ms)) {
    const int16_t delta = coalescer_.take();
    if (delta != 0) {
      push(hal::InputSource::kEncoder, hal::InputAction::kRotate, delta,
           now_ms);
    }
  }
}

bool InputManager::anyHeld() const {
  // A finger on the glass counts: it is not a wake-source pin (touch cannot
  // wake the device yet), but sleeping under a held finger would drop the
  // gesture the user is in the middle of.
  return !diag_.menu_level || !diag_.exit_level || !diag_.switch_level ||
         !diag_.encoder_a_level || !diag_.encoder_b_level || diag_.touch_down;
}

}  // namespace drivers
