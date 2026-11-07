// The DevKit synth application: UI, keybed events, voice scheduling and the audio block. Everything here runs on
// DevKitHal.h only, so nothing in this file depends on the board.
#include "DevKitApp.h"
#include "DevKitKeybed.h"
#include "DevKitHal.h"
#include "SimpleSynth.h"
#include "UiFonts.h"
#include "UiGfx.h"
#include "st7789.h"
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

// True once per press, when a button has been down for three polls.
struct ButtonState {
  int polls = 0;
};

static bool ButtonPressed(HalButton button, ButtonState &state) {
  if (!hal_button_down(button)) {
    state.polls = 0;
    return false;
  }
  return state.polls < 3 && ++state.polls == 3;
}

// Octave count shown by PianoPage. Normally the count the keybed reports
// (BinkReceiver::Octaves); the page changes the keybed's configuration with
// SETOCTAVES_* and the keybed's next announcement confirms it here.
int gDisplayOctaves = 2;
static const int kMaxKeybedOctaves = 7; // the longest Bink keybed

// Probe the keybed for its octave count and apply it to the display.
//
// The keybed answers BINK_COMMAND_SENDOCTAVES with a BINK_SPECIAL_OCTAVES
// value packet and, in UART mode, repeats that announcement once a second on
// its own. So:
//  - while no valid count has been received, ask again every 500 ms;
//  - once received, apply it to the piano layout;
//  - if the announcements stop for OCTAVE_LINK_TIMEOUT_US (keybed unplugged,
//    rebooted, or switched to USB mode), forget the count and start probing
//    again so a re-attached keybed is picked up without a DevKit reboot.
static const uint32_t OCTAVE_PROBE_INTERVAL_US = 500000;
static const uint32_t OCTAVE_LINK_TIMEOUT_US = 5000000;

static void SyncOctavesFromKeybed() {
  static uint32_t lastRequestUs = 0;
  uint32_t now = hal_time_us();

  int reported = TheKeybed.Bink.Octaves();
  if (reported > 0) {
    if (now - TheKeybed.OctavesRxUs > OCTAVE_LINK_TIMEOUT_US) {
      // Link lost: drop the stale value and fall through to probing.
      TheKeybed.Bink.ForgetOctaves();
      TheKeybed.ReleaseAll();
      reported = 0;
    } else {
      // Also puts the page right when the keybed refused a count chosen there.
      gDisplayOctaves = reported;
      return;
    }
  }

  if (now - lastRequestUs >= OCTAVE_PROBE_INTERVAL_US) {
    lastRequestUs = now;
    TheKeybed.Bink.SendCommand(BINK_COMMAND_UARTMODE); // brings it back from USB mode, and it announces itself
  }
}

// ── Parameters
// ────────────────────────────────────────────────────────────── The synth has
// 8 parameters and the board has VOLUME, the master volume, and DATA, an encoder
// that tweaks the active parameter. UP and DOWN choose which one that is, on any
// page; the header always names it and a popup shows it while it changes.
float Knob[KNOB_NUM] = {};
static const float kKnobStart[KNOB_NUM] = {0.7f, 0.7f, 0.2f, 0.25f, 0.75f, 0.5f, 0.5f, 0.0f};
static float KnobTarget[KNOB_NUM];
static const char *const KnobLabel[KNOB_NUM] = {"Volume", "Cutoff", "Reso",  "Effect",
                                                "Touch",  "Veloc",  "Curve", "Sound"};
static int gActiveParam = KNOB_CUTOFF;
static const uint32_t POPUP_TIME_US = 1500000;
static const float VOLUME_POPUP_MOVE = 0.01f;
static int gPopupParam = -1;
static uint32_t gPopupStartUs = 0;

// What the current page shows by itself needs no popup: the Synth page has all
// but the volume, the Curve page its curve.
static const int kPageShowsAll = -2, kPageShowsNone = -1;
static int gPageShows = kPageShowsAll;

static void ShowPopup(int param) {
  if (param == gPageShows || (gPageShows == kPageShowsAll && param != KNOB_VOLUME))
    return;
  gPopupParam = param;
  gPopupStartUs = hal_time_us();
}

// UP and DOWN walk the parameters; the volume has its own pot.
static void SelectParam(bool next) {
  const int count = KNOB_NUM - 1;
  gActiveParam = 1 + (gActiveParam - 1 + (next ? 1 : count - 1)) % count;
  ShowPopup(gActiveParam);
}

static void TweakParam(int param, int steps) {
  // Sound has 4 positions, the others 32 steps.
  float step = param == KNOB_SOUND ? 1.0f / 3.0f : 1.0f / 32.0f;
  float v = KnobTarget[param] + steps * step;
  KnobTarget[param] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  ShowPopup(param);
}

static void ReadVolume() {
  static float shown = -1.0f;
  float v = hal_pot_raw(HAL_POT_VOLUME) * (1.0f / 4095.0f);
  KnobTarget[KNOB_VOLUME] = v;
  if (shown < 0.0f)
    shown = v;
  if (fabsf(v - shown) > VOLUME_POPUP_MOVE) {
    shown = v;
    ShowPopup(KNOB_VOLUME);
  }
}

int DevKit_ActiveParam() { return gActiveParam; }
bool DevKit_PopupShown() { return gPopupParam >= 0; }
float DevKit_ParamValue(int param) { return KnobTarget[param]; }
const char *DevKit_ParamLabel(int param) { return KnobLabel[param]; }

// ── SimpleSynth + MIDI
// ────────────────────────────────────────────────────────

SimpleSynth Synth;

// Maps MIDI note number → active voice index (-1 = not playing)
static int noteToVoice[128];

static inline float midiNoteToFreq(uint8_t note) {
  return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// strength: 0..1.
static void StartNote(uint8_t note, float strength) {
  int voice = Synth.allocateVoice();
  Synth.updateVoiceTriggerOrder(voice);
  // Release any previous voice on this note (e.g. re-trigger without note-off)
  if (noteToVoice[note] >= 0 && noteToVoice[note] != voice)
    Synth.noteOff(noteToVoice[note]);
  // A stolen voice no longer belongs to the note it was playing.
  for (int i = 0; i < 128; i++)
    if (noteToVoice[i] == voice)
      noteToVoice[i] = -1;
  noteToVoice[note] = voice;
  Synth.noteOn(voice, midiNoteToFreq(note));
  Synth.SetOnVelocityForVoice(voice, strength); // after noteOn, which clears it
}

static void MidiNoteOff(uint8_t note) {
  int voice = noteToVoice[note];
  if (voice >= 0) {
    Synth.noteOff(voice);
    noteToVoice[note] = -1;
  }
}

// Pressure can wait for the next value; a lost note on or off cannot be made up for. So pressure leaves the last of the
// queue to the notes. Pushing must not wait for room: the audio core both fills the queue and empties it.
static const int kRoomForNotes = 32;

void SendAudioEvent(uint8_t type, uint8_t note, uint16_t value) {
  if (type == AUDIOEVENT_VALUE && hal_event_room() < kRoomForNotes)
    return;
  HalAudioEvent ev;
  ev.type = type;
  ev.note = note;
  ev.value = value;
  hal_event_push(ev);
}

// Called every main-loop iteration. Drains the USB MIDI RX queue.
// Each USB MIDI event packet is 4 bytes:
//   [0] cable (hi) | CIN (lo)   [1] status   [2] data1   [3] data2
static void PollMidi() {
  uint8_t packet[4];
  while (hal_midi_read(packet)) {
    uint8_t type = packet[0] & 0x0F; // the CIN of a note message is its status nibble
    uint8_t note = packet[2] & 0x7F;
    uint8_t vel = packet[3] & 0x7F;

    HalAudioEvent ev;
    bool valid = false;

    if (type == 0x9 && vel > 0) {
      ev.type = AUDIOEVENT_NOTEON;
      ev.note = note;
      ev.value = vel;
      valid = true;
    } else if (type == 0x8 || (type == 0x9 && vel == 0)) {
      ev.type = AUDIOEVENT_NOTEOFF;
      ev.note = note;
      ev.value = 0;
      valid = true;
    }

    if (valid) {
      hal_event_push(ev);
    }
  }
};

// ── Look ─────────────────────────────────────────────────────────────────────
// One dark ground, one accent: the accent always means "what the knobs act on".
static const uint16_t kBg = RGB(9, 11, 15);
static const uint16_t kCard = RGB(24, 28, 36);
static const uint16_t kTrack = RGB(48, 54, 66);
static const uint16_t kText = RGB(236, 239, 243);
static const uint16_t kDim = RGB(134, 142, 154);
static const uint16_t kAccent = RGB(255, 204, 41);
static const uint16_t kCool = RGB(86, 204, 242);

static const int kHeaderH = 20;
static const int kRowH = 16;
static const int kScreenW = 240;

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ── Keyboard ─────────────────────────────────────────────────────────────────
// The whole keybed as one picture; a key fills with the accent as it goes down.
static const bool kIsBlack[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};

static float KeyLevel(int key) { return Clamp01(TheKeybed.Bink.Position(key) / 700.0f); }

static void DrawKeyboard(int y, int h) {
  const int octaves = TheKeybed.Bink.Octaves() > 0 ? TheKeybed.Bink.Octaves() : gDisplayOctaves;
  const int keys = octaves * 12 + 1;
  const int whites = octaves * 7 + 1;
  const int kw = (kScreenW - 4) / whites;
  const int x0 = (kScreenW - kw * whites) / 2;
  const int bw = kw * 5 / 8 < 3 ? 3 : kw * 5 / 8, bh = h * 3 / 5;
  int white = 0;
  for (int k = 0; k < keys; k++) {
    if (kIsBlack[k % 12])
      continue;
    ui_fill_round(x0 + white * kw, y, kw - 1, h, 1, ui_blend(RGB(206, 211, 218), kAccent, (int)(KeyLevel(k) * 255)));
    white++;
  }
  white = 0;
  for (int k = 0; k < keys; k++) {
    if (!kIsBlack[k % 12]) {
      white++;
      continue;
    }
    int x = x0 + white * kw - bw / 2 - 1;
    ui_fill(x - 1, y, bw + 2, bh + 1, kBg);
    ui_fill_round(x, y, bw, bh, 1, ui_blend(RGB(44, 50, 60), kAccent, (int)(KeyLevel(k) * 255)));
  }
}

// ── Pages ────────────────────────────────────────────────────────────────────
// Two states. On the page bar LEFT and RIGHT flip through the pages. ENTER goes
// into a page, where they do what StepHint() says, and BACK comes out. The header
// shows which it is. A page without a StepHint has nothing to go into.
class Page {
public:
  char Title[16] = "No Title";
  void SetTitle(const char *title) {
    snprintf(Title, sizeof(Title), "%s", title);
  }

  virtual const char *StepHint() { return nullptr; }
  virtual int Shows() { return kPageShowsNone; } // see ShowPopup
  virtual void Step(bool next) {}
  virtual void Trigger() {} // ENTER on a page with no StepHint
  virtual void Render(bool inside) {}
};

// A line of a page: a card behind it, the accent while LEFT and RIGHT act on it.
static void DrawRow(const char *label, const char *value, int y, bool inside) {
  if (inside)
    ui_fill_round(2, y, kScreenW - 4, kRowH - 1, 4, kCard);
  ui_text(8, y + 1, label, kFontSmall, inside ? kAccent : kDim);
  if (value)
    ui_text_right(kScreenW - 8, y + 1, value, kFontSmall, inside ? kAccent : kText);
}

static const int kContentY = kHeaderH + 2;

class PianoPage : public Page {
public:
  PianoPage() { SetTitle("Keys"); }

  virtual const char *StepHint() { return "octaves"; }

  virtual void Step(bool next) {
    int octaves = gDisplayOctaves + (next ? 1 : -1);
    if (octaves < 1)
      octaves = kMaxKeybedOctaves;
    else if (octaves > kMaxKeybedOctaves)
      octaves = 1;
    // Show the requested count right away; the keybed's confirmation (or a
    // different count, if it refused) arrives through SyncOctavesFromKeybed.
    gDisplayOctaves = octaves;
    TheKeybed.Bink.SetOctaves(octaves);
  }

  virtual void Render(bool inside) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", gDisplayOctaves);
    DrawRow("Octaves", buf, kContentY, inside);
    DrawKeyboard(42, 90);
  }
};

class TemperaturePage : public Page {
public:
  TemperaturePage() { SetTitle("Temp"); }

  virtual void Trigger() { TheKeybed.Bink.RequestTemperatures(); }

  virtual void Render(bool inside) {
    DrawRow("Measure", "ENTER", kContentY, false);
    char buf[16];
    for (int i = 0; i < 16; i++) {
      int x = 4 + (i % 4) * 59, y = 40 + (i / 4) * 24;
      float t = TheKeybed.Bink.Temperature(i);
      ui_fill_round(x, y, 55, 21, 4, kCard);
      snprintf(buf, sizeof(buf), "%d", i);
      ui_text(x + 4, y + 4, buf, kFontSmall, kDim);
      if (t > -20)
        snprintf(buf, sizeof(buf), "%.1f", t);
      else
        snprintf(buf, sizeof(buf), "-");
      ui_text_right(x + 51, y + 4, buf, kFontSmall, kText);
    }
  }
};

class SensorPage : public Page {
public:
  SensorPage() { SetTitle("Sensors"); }
  int Octave = 0;

  virtual const char *StepHint() { return "octave"; }

  virtual void Step(bool next) { Octave = (Octave + (next ? 1 : -1) + BINK_MAX_OCTAVES) % BINK_MAX_OCTAVES; }

  virtual void Render(bool inside) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", Octave);
    DrawRow("Octave", buf, kContentY, inside);
    for (int i = 0; i < 12; i++) {
      int x = 4 + (i % 3) * 78, y = 40 + (i / 3) * 24;
      const int value = TheKeybed.Bink.Position(i + Octave * 12);
      ui_fill_round(x, y, 75, 21, 4, kCard);
      ui_fill_round(x, y + 18, (int)(Clamp01(value / 700.0f) * 75), 3, 1, kAccent);
      snprintf(buf, sizeof(buf), "%d", i + Octave * 12);
      ui_text(x + 5, y + 3, buf, kFontSmall, kDim);
      snprintf(buf, sizeof(buf), "%d", value);
      ui_text_right(x + 70, y + 3, buf, kFontSmall, kText);
    }
  }
};

static void ParamText(int param, char *buf, size_t size);

// All the parameters. The accent row is the active one, here as everywhere:
// UP and DOWN move it, DATA and (inside the page) LEFT and RIGHT set its value.
class SynthPage : public Page {
public:
  SynthPage() { SetTitle("Synth"); }

  virtual const char *StepHint() { return "value"; }
  virtual int Shows() { return kPageShowsAll; }
  virtual void Step(bool next) { TweakParam(gActiveParam, next ? 1 : -1); }

  virtual void Render(bool inside) {
    char value[24];
    for (int idx = 1; idx < KNOB_NUM; idx++) {
      int y = kContentY + (idx - 1) * kRowH;
      bool active = idx == gActiveParam;
      if (active)
        ui_fill_round(2, y, kScreenW - 4, kRowH - 1, 4, kCard);
      ui_text(8, y + 1, KnobLabel[idx], kFontSmall, active ? kAccent : kDim);
      ParamText(idx, value, sizeof(value));
      ui_text_right(kScreenW - 8, y + 1, value, kFontSmall, active ? kAccent : kText);
      const int bx = 62, bw = 104;
      uint16_t col = active ? kAccent : kDim;
      if (idx == KNOB_TOUCH) {
        ui_bar_centered(bx, y + 6, bw, 3, KnobTarget[idx], kTrack, col);
      } else if (idx == KNOB_SOUND) {
        int sel = (int)(KnobTarget[idx] * 3.999f);
        for (int i = 0; i < SYNTH_NUM_MODES; i++)
          ui_fill_round(bx + i * (bw / 4), y + 6, bw / 4 - 3, 3, 1, i == sel ? col : kTrack);
      } else {
        ui_bar(bx, y + 6, bw, 3, KnobTarget[idx], kTrack, col);
      }
    }
  }
};

// The touch response curve with the live voices on it.
class CurvePage : public Page {
public:
  CurvePage() { SetTitle("Curve"); }

  virtual const char *StepHint() { return "curve"; }
  virtual int Shows() { return KNOB_CURVE; }
  virtual void Step(bool next) { TweakParam(KNOB_CURVE, next ? 1 : -1); }

  virtual void Render(bool inside) {
    const int x = 8, y = kHeaderH + 4, w = kScreenW - 16, h = 135 - y - 5;
    ui_fill_round(x, y, w, h, 5, kCard);
    for (int i = 1; i < 4; i++) {
      ui_fill(x + w * i / 4, y + 3, 1, h - 6, kTrack);
      ui_fill(x + 3, y + h * i / 4, w - 6, 1, kTrack);
    }
    int prev = -1;
    for (int i = 4; i < w - 4; i++) {
      float n = (float)(i - 4) / (float)(w - 9);
      int py = y + h - 5 - (int)(Synth.applyCurve(n) * (h - 10));
      if (prev < 0)
        prev = py;
      int top = py < prev ? py : prev, bottom = py < prev ? prev : py;
      ui_fill(x + i, top, 1, bottom - top + 2, kAccent);
      prev = py;
    }
    for (int i = 0; i < kNumVoices; i++)
      if (Synth.voices[i].active) {
        float p = Clamp01(Synth.voices[i].pressure);
        ui_dot(x + 4 + (int)(p * (w - 9)), y + h - 5 - (int)(Synth.applyCurve(p) * (h - 10)), 3, kCool);
      }
  }
};

Page *Pages[] = {new SynthPage(), new PianoPage(), new CurvePage(), new SensorPage(), new TemperaturePage()};

// A parameter's value as it reads on screen.
static void ParamText(int param, char *buf, size_t size) {
  float v = KnobTarget[param];
  if (param == KNOB_SOUND)
    snprintf(buf, size, "%s", Synth.getModeName());
  else if (param == KNOB_TOUCH)
    snprintf(buf, size, "%+d", (int)(v * 200.0f + 0.5f) - 100);
  else
    snprintf(buf, size, "%d", (int)(v * 100.0f + 0.5f));
}

// Header, right side: the parameter DATA is on, on every page.
static void DrawActiveParam() {
  char value[24];
  ParamText(gActiveParam, value, sizeof(value));
  int vw = ui_text_width(value, kFontSmall);
  ui_text(kScreenW - 6 - vw, 3, value, kFontSmall, kAccent);
  ui_text_right(kScreenW - 6 - vw - 6, 3, KnobLabel[gActiveParam], kFontSmall, kDim);
}

// Over everything for a moment after a parameter is touched.
static void DrawPopup() {
  if (gPopupParam < 0)
    return;
  if (hal_time_us() - gPopupStartUs > POPUP_TIME_US) {
    gPopupParam = -1;
    return;
  }
  const int w = 172, h = 88, x = (kScreenW - w) / 2, y = 30;
  char value[24];
  ParamText(gPopupParam, value, sizeof(value));
  ui_fill_round(x - 2, y - 2, w + 4, h + 4, 9, kBg);
  ui_fill_round(x, y, w, h, 7, kCard);
  ui_text(x + 10, y + 7, gPopupParam == KNOB_VOLUME ? "VOLUME" : "DATA", kFontSmall, kDim);
  ui_text_right(x + w - 10, y + 7, KnobLabel[gPopupParam], kFontSmall, kText);
  // The big digits have no letters: a sound's name stays in the title size.
  if (gPopupParam == KNOB_SOUND)
    ui_text_center(x + w / 2, y + 34, value, kFontTitle, kAccent);
  else
    ui_text_center(x + w / 2, y + 22, value, kFontBig, kAccent);
  if (gPopupParam == KNOB_TOUCH)
    ui_bar_centered(x + 10, y + h - 13, w - 20, 5, KnobTarget[gPopupParam], kTrack, kAccent);
  else
    ui_bar(x + 10, y + h - 13, w - 20, 5, KnobTarget[gPopupParam], kTrack, kAccent);
}

// A small arrow, 4 wide, centred on y.
static void DrawArrow(int x, int y, bool left, uint16_t color) {
  for (int i = 0; i < 4; i++) {
    int reach = left ? i : 3 - i;
    ui_fill(x + i, y - reach, 1, reach * 2 + 1, color);
  }
}

class UIState {
public:
  int PageIndex = 0;
  bool Inside = false; // in a page: LEFT and RIGHT do what it says
  Page *CurrentPage = nullptr;
  UIState() { CurrentPage = Pages[0]; }

  void NextPrev(bool Next) {
    if (Inside) {
      CurrentPage->Step(Next);
      return;
    }
    const int numPages = sizeof(Pages) / sizeof(Page *);
    PageIndex = (PageIndex + (Next ? 1 : -1) + numPages) % numPages;
    CurrentPage = Pages[PageIndex];
    gPageShows = CurrentPage->Shows();
  }

  void Cancel() { Inside = false; }

  void Enter() {
    if (CurrentPage->StepHint())
      Inside = true;
    else
      CurrentPage->Trigger();
  }

  void Render() {
    // The arrows sit by what LEFT and RIGHT act on: the page's name on the page
    // bar, inside a page a tag that says what they change.
    int x = 4;
    if (!Inside) {
      DrawArrow(x, 9, true, kAccent);
      x += 7;
      x = ui_text(x, 1, CurrentPage->Title, kFontTitle, kText) + 3;
      DrawArrow(x, 9, false, kAccent);
      x += 9;
      const int numPages = sizeof(Pages) / sizeof(Page *);
      for (int i = 0; i < numPages; i++)
        ui_fill_round(x + i * 6, 8, 4, 4, 2, i == PageIndex ? kText : kTrack);
    } else {
      x = ui_text(x + 1, 1, CurrentPage->Title, kFontTitle, kDim) + 6;
      const char *hint = CurrentPage->StepHint();
      int w = ui_text_width(hint, kFontSmall) + 20;
      ui_fill_round(x, 2, w, 15, 5, kAccent);
      DrawArrow(x + 4, 9, true, kBg);
      ui_text(x + 10, 2, hint, kFontSmall, kBg);
      DrawArrow(x + w - 8, 9, false, kBg);
    }
    DrawActiveParam();
    ui_fill(0, kHeaderH - 1, kScreenW, 1, kTrack);

    CurrentPage->Render(Inside);
    DrawPopup();
  }
};

UIState UI;

// Drain queued audio events and apply them to the synth (note on/off,
// per-voice pressure). Runs on core 1 only.
static inline void __not_in_flash_func(DrainAudioEventsIntoSynth)() {
  HalAudioEvent ev;
  while (hal_event_pop(ev)) {
    switch (ev.type) {
    case AUDIOEVENT_NOTEON:
      StartNote(ev.note, ev.value / 127.0f);
      break;
    case AUDIOEVENT_NOTEOFF:
      MidiNoteOff(ev.note);
      break;
    case AUDIOEVENT_VALUE: {
      int voice = noteToVoice[ev.note];
      if (voice >= 0) {
        float cutoff = (float)ev.value / 1500.0f;
        if (cutoff > 1.0f)
          cutoff = 1.0f;
        if (cutoff < 0.0f)
          cutoff = 0.0f;
        Synth.setVoicePressure(voice, cutoff);
      }
    } break;
    case AUDIOEVENT_NOTEONVELOCITY:
      StartNote(ev.note, Clamp01(ev.value / 400.0f));
      break;
    case AUDIOEVENT_NOTEOFFVELOCITY: {
      int voice = noteToVoice[ev.note];
      if (voice > -1)
        Synth.SetOffVelocityForVoice(voice, Clamp01(ev.value / 400.0f));
      MidiNoteOff(ev.note);
    } break;
    }
  }
}


static bool g_pipelineWarm = false;

// The wordmark as on the board, under the four brand dots. They dance: each hops
// in a wave that runs along the word and dies down, and it all lands in place.
static const float kBootDanceSeconds = 1.8f;
static const float kBootFadeSeconds = 1.0f;
static const float kBootSeconds = 2.3f;

static int BootHop(float t, int index, float height) {
  float left = 1.0f - t / kBootDanceSeconds;
  if (left <= 0.0f)
    return 0;
  return (int)(-fabsf(sinf(t * 7.0f - index * 0.6f)) * height * left);
}

static void DrawBootScreen(float t) {
  static const uint16_t dots[4] = {RGB(0, 174, 239), RGB(236, 0, 140), RGB(255, 222, 0), RGB(236, 239, 243)};
  static const char word[] = "BINK DEVKIT";
  st7789_clear_screen(kBg);
  const int gap = 13, y = 42;
  for (int i = 0; i < 4; i++)
    ui_dot(kScreenW / 2 + (i * 2 - 3) * gap / 2, y + BootHop(t, i + 3, 6.0f), 4, dots[i]);
  int x = kScreenW / 2 - ui_text_width(word, kFontBrand) / 2;
  for (int i = 0; word[i]; i++) {
    char one[2] = {word[i], 0};
    ui_text(x, 58 + BootHop(t, i, 12.0f), one, kFontBrand, kText);
    x += ui_text_width(one, kFontBrand);
  }
  st7789_display();
}

static void PlayBootScreen() {
  const uint32_t start = hal_time_us();
  for (;;) {
    float t = (hal_time_us() - start) * 1e-6f;
    if (t > kBootSeconds)
      break;
    hal_backlight(t >= kBootFadeSeconds ? 255 : (int)(t / kBootFadeSeconds * 255.0f));
    DrawBootScreen(t);
    hal_sleep_ms(16);
  }
}

void DevKit_Reset() {
  for (int i = 0; i < KNOB_NUM; i++) {
    KnobTarget[i] = kKnobStart[i];
    Knob[i] = 0;
  }
  gActiveParam = KNOB_CUTOFF;
  gPopupParam = -1;
  gPageShows = kPageShowsAll;
  gDisplayOctaves = 2;
  UI = UIState();
  new (&Synth) SimpleSynth();
  new (&TheKeybed) DevKitKeybed();
  g_pipelineWarm = false;
  st7789_forget_panel();
}

void DevKit_WorkerStep() {
  // Drain raw keybed bytes and decode them into protocol events
  TheKeybed.Drain();
}

int DevKit_Run() {
  memcpy(KnobTarget, kKnobStart, sizeof(KnobTarget));

  // Mark all MIDI notes as unassigned
  for (int i = 0; i < 128; i++)
    noteToVoice[i] = -1;

  st7789_begin();
  PlayBootScreen();

  ButtonState btn_back, btn_left, btn_right, btn_enter, btn_up, btn_down;

  hal_audio_start();

  hal_sleep_ms(10);
  TheKeybed.Init();
  hal_sleep_ms(10);
  // Ask the keybed how many octaves it is configured for rather than forcing
  // a value; SyncOctavesFromKeybed() in the main loop retries and applies it.
  TheKeybed.Bink.RequestOctaves();

  hal_worker_start();

  while (true) {
    hal_usb_task();
    PollMidi();
    SyncOctavesFromKeybed();

    // Pages: LEFT, RIGHT, ENTER, BACK.
    if (ButtonPressed(HAL_BTN_ENTER, btn_enter)) {
      UI.Enter();
    }
    if (ButtonPressed(HAL_BTN_BACK, btn_back)) {
      UI.Cancel();
    }
    if (ButtonPressed(HAL_BTN_RIGHT, btn_right)) {
      UI.NextPrev(true);
    }
    if (ButtonPressed(HAL_BTN_LEFT, btn_left)) {
      UI.NextPrev(false);
    }

    // The parameter: UP, DOWN choose it, DATA tweaks it.
    if (ButtonPressed(HAL_BTN_UP, btn_up)) {
      SelectParam(false);
    }
    if (ButtonPressed(HAL_BTN_DOWN, btn_down)) {
      SelectParam(true);
    }
    int steps = hal_data_steps();
    if (steps != 0) {
      TweakParam(gActiveParam, steps);
    }
    ReadVolume();

    st7789_clear_screen(kBg);
    UI.Render();
    st7789_display();
    for (int i = 0; i < 100; i++) {
      hal_usb_task();
      PollMidi();
      hal_sleep_us(100);
    }
  }
  return 0;
}

// USB CDC was replaced by MIDI; provide a no-op so BinkComm still links.

// ── Audio block ──────────────────────────────────────────────────────────────
//
// Output words are I2S ready, R0, L0, R1, L1, ... left-justified 16 bit samples
// (the AIC3204 takes the upper 16 bits of each 32-bit word).
//
// One-pole smoothing of the parameters: the block rate is ~3 kHz, alpha 0.02
// gives ~10 Hz, enough to follow the pots without zipper noise.
#define KNOB_SMOOTH 0.02f

// Voices 8..11 render on the other core, one block ahead: hal_render_request()
// hands DevKit_RenderOffloaded() to core 0 (on the board: the SIO FIFO IRQ, so it
// preempts the UI) and the next block's hal_render_join() collects it. It may
// still be running while this core renders voices 0..7: they share no state.
#define MAX_AUDIO_FRAMES 32
#define OFFLOAD_FIRST_VOICE 8

static float g_scratchOffload[MAX_AUDIO_FRAMES];
static float g_scratchLocal[MAX_AUDIO_FRAMES];
static uint32_t g_offloadFrames = 0;

void __not_in_flash_func(DevKit_RenderOffloaded)() {
  for (uint32_t i = 0; i < g_offloadFrames; i++)
    g_scratchOffload[i] = 0.0f;
  Synth.renderVoiceRangeAdd(g_scratchOffload, g_offloadFrames, OFFLOAD_FIRST_VOICE, kNumVoices);
}

void __not_in_flash_func(process_audio)(const uint32_t *input, uint32_t *output, size_t num_frames) {
  (void)input;

  if (num_frames > MAX_AUDIO_FRAMES)
    num_frames = MAX_AUDIO_FRAMES;

  // 1. Render voices 0..7 here for THIS block; the other core is working on
  //    voices 8..11 for this block, started at the end of the previous one.
  for (size_t i = 0; i < num_frames; i++)
    g_scratchLocal[i] = 0.0f;
  Synth.renderVoiceRangeAdd(g_scratchLocal, num_frames, 0, OFFLOAD_FIRST_VOICE);

  // 2. Collect them. The very first call has no job to collect.
  if (g_pipelineWarm) {
    hal_render_join();
  } else {
    for (size_t i = 0; i < num_frames; i++)
      g_scratchOffload[i] = 0.0f;
  }

  // 3. Sum, delay, write I2S words for THIS block.
  for (size_t i = 0; i < num_frames; i++)
    g_scratchLocal[i] += g_scratchOffload[i];
  Synth.finalizeBlockI2S(g_scratchLocal, (int32_t *)output, num_frames);

  // 4. Apply pending events and parameter changes: the other core is idle, so
  //    this is the only safe window to mutate voice state.
  DrainAudioEventsIntoSynth();
  for (int k = 0; k < KNOB_NUM; k++)
    Knob[k] += KNOB_SMOOTH * (KnobTarget[k] - Knob[k]);
  Synth.setKnobs(Knob);
  Synth.prepareBlock();

  // 5. Kick off the NEXT block's offloaded voices.
  g_offloadFrames = (uint32_t)num_frames;
  hal_render_request();
  g_pipelineWarm = true;
}
