#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <SpotifyArduino.h>
#include <ArduinoJson.h>
#include <JPEGDecoder.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <math.h>

// ─── CREDENTIALS ──────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* CLIENT_ID     = "YOUR_SPOTIFY_CLIENT_ID";
const char* CLIENT_SECRET = "YOUR_SPOTIFY_CLIENT_SECRET";
const char* REFRESH_TOKEN = "YOUR_SPOTIFY_REFRESH_TOKEN";
// ──────────────────────────────────────────────────────────────

// ─── PINS ─────────────────────────────────────────────────────
#define TOUCH_CS  33
#define TOUCH_IRQ 36
// ──────────────────────────────────────────────────────────────

// ─── SPOTIFY COLOURS ──────────────────────────────────────────
#define COL_BG       0x0000
#define COL_ACCENT   0x06C0
#define COL_TEXT     0xFFFF
#define COL_SUBTEXT  0xAD55
#define COL_BTN      0x2124
#define COL_PROGBG   0x2124
#define COL_VOLBG    0x2124
// ──────────────────────────────────────────────────────────────

// ─── MOON COLOURS ─────────────────────────────────────────────
#define COL_MOON_LIGHT   0xEF5C  // bright warm white highlands
#define COL_MOON_DARK    0xA534  // maria
#define COL_MOON_CRATER  0xFFFF  // crater rims
#define COL_SHADOW       0x0000
#define COL_SPACE        0x0000
// ──────────────────────────────────────────────────────────────

// ─── BUTTON ZONES (landscape 320x240) ─────────────────────────
#define BTN_PREV_X1   10
#define BTN_PREV_X2   90
#define BTN_PLAY_X1  115
#define BTN_PLAY_X2  205
#define BTN_NEXT_X1  230
#define BTN_NEXT_X2  310
#define BTN_Y1       195
#define BTN_Y2       235

#define VOL_X1        30
#define VOL_X2       290
#define VOL_Y1       170
#define VOL_Y2       188
// ──────────────────────────────────────────────────────────────

// ─── MOON CONSTANTS ───────────────────────────────────────────
#define MOON_X  160
#define MOON_Y  108
#define MOON_R   88
// ──────────────────────────────────────────────────────────────

// ─── OBJECTS ──────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
WiFiClientSecure client;
SpotifyArduino spotify(client, CLIENT_ID, CLIENT_SECRET, REFRESH_TOKEN);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
// ──────────────────────────────────────────────────────────────

// ─── SPOTIFY STATE ────────────────────────────────────────────
String lastTrack        = "";
String lastArtist       = "";
String lastAlbumArtUrl  = "";
bool   isPlaying        = false;
int    trackDurationMs  = 0;
int    trackProgressMs  = 0;
int    currentVolume    = 50;
unsigned long lastPoll          = 0;
unsigned long lastProgressSync  = 0;
const unsigned long POLL_INTERVAL = 3000;
// ──────────────────────────────────────────────────────────────

// ─── MOON STATE ───────────────────────────────────────────────
float librationAngle  = 0.0;
float lastMoonPhase   = -1.0;
float lastLibAngle    = -999.0;
unsigned long lastMoonDraw    = 0;
unsigned long lastLibUpdate   = 0;
bool starsGenerated   = false;

struct Star {
  int x, y;
  uint8_t brightness;
};
#define NUM_STARS 80
Star stars[NUM_STARS];
// ──────────────────────────────────────────────────────────────

// ═══════════════════════════════════════════════════════════════
//  SPOTIFY DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════════

// ─── JPEG RENDERING ───────────────────────────────────────────
void renderJPEG(int xOffset, int yOffset) {
  uint16_t* pImg;
  while (JpegDec.readSwappedBytes()) {
    pImg     = JpegDec.pImage;
    int imgX = JpegDec.MCUx * JpegDec.MCUWidth  + xOffset;
    int imgY = JpegDec.MCUy * JpegDec.MCUHeight + yOffset;
    int imgW = JpegDec.MCUWidth;
    int imgH = JpegDec.MCUHeight;
    if (imgX + imgW > 320) imgW = 320 - imgX;
    if (imgY + imgH > 240) imgH = 240 - imgY;
    if (imgW < 1 || imgH < 1) continue;
    tft.pushImage(imgX, imgY, imgW, imgH, pImg);
  }
}

bool downloadAndDrawAlbumArt(String url) {
  if (url.isEmpty()) return false;
  HTTPClient http;
  WiFiClientSecure artClient;
  artClient.setInsecure();
  http.begin(artClient, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("Album art fetch failed: %d\n", httpCode);
    http.end();
    return false;
  }
  int imageSize      = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  uint8_t* imageBuffer = (uint8_t*)malloc(imageSize);
  if (!imageBuffer) {
    Serial.println("Not enough RAM for album art");
    http.end();
    return false;
  }
  int bytesRead = 0;
  while (http.connected() && bytesRead < imageSize) {
    int available = stream->available();
    if (available)
      bytesRead += stream->readBytes(imageBuffer + bytesRead, available);
    delay(1);
  }
  http.end();
  JpegDec.decodeArray(imageBuffer, bytesRead);
  int xOffset = (320 - 150) / 2;
  int yOffset = 15;
  renderJPEG(xOffset, yOffset);
  free(imageBuffer);
  return true;
}

// ─── PROGRESS BAR ─────────────────────────────────────────────
void drawProgressBar() {
  int barX = 30;
  int barY = 148;
  int barW = 200;
  int barH = 5;
  tft.fillRoundRect(barX, barY, barW, barH, 3, COL_PROGBG);
  if (trackDurationMs > 0) {
    int filled = map(trackProgressMs, 0, trackDurationMs, 0, barW);
    filled = constrain(filled, 0, barW);
    tft.fillRoundRect(barX, barY, filled, barH, 3, COL_ACCENT);
  }
  int elapsed  = trackProgressMs / 1000;
  int duration = trackDurationMs / 1000;
  char elapsedStr[6], durationStr[6];
  sprintf(elapsedStr,  "%d:%02d", elapsed  / 60, elapsed  % 60);
  sprintf(durationStr, "%d:%02d", duration / 60, duration % 60);
  tft.setTextFont(1);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(elapsedStr,  barX,        barY + 10);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(durationStr, barX + barW, barY + 10);
}

// ─── VOLUME BAR ───────────────────────────────────────────────
void drawVolumeBar() {
  int barX = VOL_X1;
  int barY = VOL_Y1;
  int barW = VOL_X2 - VOL_X1;
  int barH = 5;
  tft.setTextFont(1);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("VOL", barX - 28, barY + 2);
  tft.fillRoundRect(barX, barY, barW, barH, 3, COL_VOLBG);
  int filled = map(currentVolume, 0, 100, 0, barW);
  filled = constrain(filled, 0, barW);
  tft.fillRoundRect(barX, barY, filled, barH, 3, COL_ACCENT);
  char volStr[5];
  sprintf(volStr, "%d%%", currentVolume);
  tft.fillRect(VOL_X2 + 3, barY - 3, 30, 12, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.drawString(volStr, VOL_X2 + 3, barY + 2);
}

void adjustVolume(int newVolume) {
  newVolume = constrain(newVolume, 0, 100);
  if (newVolume == currentVolume) return;
  currentVolume = newVolume;
  spotify.setVolume(currentVolume);
  drawVolumeBar();
  Serial.printf("Volume set to %d\n", currentVolume);
}

// ─── BUTTONS ──────────────────────────────────────────────────
void drawButtons(bool playing) {
  tft.fillRoundRect(BTN_PREV_X1, BTN_Y1, 80, 38, 8, COL_BTN);
  tft.fillRoundRect(BTN_PLAY_X1, BTN_Y1, 90, 38, 8, COL_ACCENT);
  tft.fillRoundRect(BTN_NEXT_X1, BTN_Y1, 80, 38, 8, COL_BTN);
  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("|<",               50, 214);
  tft.drawString(playing ? "||" : ">", 160, 214);
  tft.drawString(">|",              270, 214);
}

// ─── TRACK INFO ───────────────────────────────────────────────
void drawTrackInfo(String track, String artist) {
  tft.fillRect(0, 125, 320, 40, COL_BG);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.drawString(artist.substring(0, 40), 160, 133);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextFont(2);
  String displayTrack = track.length() > 28
    ? track.substring(0, 25) + "..."
    : track;
  tft.drawString(displayTrack, 160, 144);
}

// ─── STATUS MESSAGE ───────────────────────────────────────────
void drawStatusMessage(String msg) {
  tft.fillRect(0, 0, 320, 14, COL_BG);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.drawString(msg, 160, 7);
}

// ─── SPOTIFY CALLBACK ─────────────────────────────────────────
void trackCallback(CurrentlyPlaying current) {
  String track  = String(current.trackName);
  String artist = String(current.artists[0].artistName);
  String artUrl = String(current.albumImages[1].url);
  isPlaying        = current.isPlaying;
  trackDurationMs  = current.durationMs;
  trackProgressMs  = current.progressMs;
  lastProgressSync = millis();

  if (current.volumePercent != currentVolume)
    currentVolume = current.volumePercent;

  bool trackChanged = (track != lastTrack || artist != lastArtist);

  if (trackChanged) {
    lastTrack  = track;
    lastArtist = artist;
    tft.fillScreen(COL_BG);
    if (artUrl != lastAlbumArtUrl) {
      lastAlbumArtUrl = artUrl;
      drawStatusMessage("Loading art...");
      downloadAndDrawAlbumArt(artUrl);
    }
    drawTrackInfo(track, artist);
    drawButtons(isPlaying);
  } else {
    drawButtons(isPlaying);
  }

  drawProgressBar();
  drawVolumeBar();
  Serial.println("Now playing: " + track + " - " + artist);
}

// ═══════════════════════════════════════════════════════════════
//  MOON DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════════

// ─── STARFIELD ────────────────────────────────────────────────
void generateStars() {
  if (starsGenerated) return;
  randomSeed(42);
  for (int i = 0; i < NUM_STARS; i++) {
    int x, y;
    do {
      x = random(0, 320);
      y = random(0, 220);
    } while (
      sqrt((float)((x - MOON_X) * (x - MOON_X) +
                   (y - MOON_Y) * (y - MOON_Y))) < MOON_R + 6
    );
    stars[i].x          = x;
    stars[i].y          = y;
    stars[i].brightness = random(1, 4);
  }
  starsGenerated = true;
}

void drawStars() {
  for (int i = 0; i < NUM_STARS; i++) {
    uint16_t col;
    switch (stars[i].brightness) {
      case 1:  col = 0x4208; break;  // dim
      case 2:  col = 0x8C71; break;  // medium
      default: col = 0xFFFF; break;  // bright
    }
    tft.drawPixel(stars[i].x, stars[i].y, col);
    if (stars[i].brightness == 3) {
      tft.drawPixel(stars[i].x + 1, stars[i].y, col);
      tft.drawPixel(stars[i].x - 1, stars[i].y, col);
      tft.drawPixel(stars[i].x, stars[i].y + 1, col);
      tft.drawPixel(stars[i].x, stars[i].y - 1, col);
    }
  }
}

// ─── LUNAR PHASE ──────────────────────────────────────────────
float getLunarPhase() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0.5;
  time_t now;
  time(&now);
  double daysSinceRef = (now - 946941240.0) / 86400.0;
  double cycle = fmod(daysSinceRef, 29.53059);
  return (float)(cycle / 29.53059);
}

String getPhaseName(float phase) {
  if (phase < 0.03 || phase > 0.97) return "New Moon";
  if (phase < 0.22) return "Waxing Crescent";
  if (phase < 0.28) return "First Quarter";
  if (phase < 0.47) return "Waxing Gibbous";
  if (phase < 0.53) return "Full Moon";
  if (phase < 0.72) return "Waning Gibbous";
  if (phase < 0.78) return "Last Quarter";
  return "Waning Crescent";
}

// ─── MOON SURFACE ─────────────────────────────────────────────
void drawMoonSurface(int cx, int cy, int r, int libX, int libY) {
  // Base highlands
  tft.fillCircle(cx, cy, r, COL_MOON_LIGHT);

  // Maria — drawn as filled ellipses via horizontal scanlines
  struct Maria { float ox, oy, rx, ry; uint16_t col; };
  Maria maria[] = {
    { 0.15f,  0.10f, 0.22f, 0.18f, COL_MOON_DARK },  // Mare Tranquillitatis
    { 0.10f, -0.15f, 0.16f, 0.14f, COL_MOON_DARK },  // Mare Serenitatis
    {-0.18f, -0.20f, 0.25f, 0.22f, COL_MOON_DARK },  // Mare Imbrium
    { 0.38f,  0.05f, 0.10f, 0.08f, COL_MOON_DARK },  // Mare Crisium
    {-0.25f,  0.10f, 0.28f, 0.32f, 0x9CF3        },  // Oceanus Procellarum
    {-0.10f,  0.30f, 0.18f, 0.14f, COL_MOON_DARK },  // Mare Nubium
    { 0.28f,  0.22f, 0.14f, 0.12f, COL_MOON_DARK },  // Mare Fecunditatis
    {-0.22f,  0.32f, 0.10f, 0.09f, COL_MOON_DARK },  // Mare Humorum
  };

  for (auto& m : maria) {
    int mx  = cx + (int)(m.ox * r) + libX;
    int my  = cy + (int)(m.oy * r) + libY;
    int mrx = (int)(m.rx * r);
    int mry = (int)(m.ry * r);
    for (int dy = -mry; dy <= mry; dy++) {
      float ratio = 1.0f - ((float)(dy * dy) / (float)(mry * mry));
      if (ratio < 0) continue;
      int dx = (int)(mrx * sqrt(ratio));
      int py = my + dy;
      if (py < 0 || py >= 240) continue;
      int x1 = constrain(mx - dx, cx - r, cx + r);
      int x2 = constrain(mx + dx, cx - r, cx + r);
      if (x2 > x1) tft.drawFastHLine(x1, py, x2 - x1, m.col);
    }
  }

  // Craters
  struct Crater { float ox, oy, cr; };
  Crater craters[] = {
    {-0.05f,  0.42f, 0.06f},  // Tycho
    {-0.18f,  0.12f, 0.07f},  // Copernicus
    {-0.30f,  0.10f, 0.04f},  // Kepler
    {-0.38f, -0.05f, 0.05f},  // Aristarchus
    {-0.12f, -0.38f, 0.05f},  // Plato
    {-0.44f,  0.18f, 0.06f},  // Grimaldi
    {-0.08f,  0.52f, 0.09f},  // Clavius
    { 0.40f,  0.25f, 0.05f},  // Langrenus
    { 0.42f,  0.35f, 0.05f},  // Petavius
  };

  for (auto& c : craters) {
    float dist = sqrt(c.ox * c.ox + c.oy * c.oy);
    if (dist > 0.85f) continue;
    int cx2 = cx + (int)(c.ox * r) + libX;
    int cy2 = cy + (int)(c.oy * r) + libY;
    int cr  = max(2, (int)(c.cr * r));
    tft.fillCircle(cx2, cy2, cr, 0x7BCF);
    tft.drawCircle(cx2, cy2, cr,     COL_MOON_CRATER);
    tft.drawCircle(cx2, cy2, cr + 1, 0xDEDB);
  }

  // Clip outside moon disk with space colour
  for (int dy = -r; dy <= r; dy++) {
    int chordW = (int)sqrt((float)(r * r - dy * dy));
    int py = cy + dy;
    if (py < 0 || py >= 240) continue;
    if (cx - chordW > 0)
      tft.drawFastHLine(0, py, cx - chordW, COL_SPACE);
    if (cx + chordW < 320)
      tft.drawFastHLine(cx + chordW, py, 320 - (cx + chordW), COL_SPACE);
  }
}

// ─── PHASE SHADOW ─────────────────────────────────────────────
void drawPhaseShadow(int cx, int cy, int r, float phase) {
  if (phase < 0.02f || phase > 0.98f) {
    tft.fillCircle(cx, cy, r, COL_SHADOW);
    return;
  }
  if (phase > 0.48f && phase < 0.52f) return; // full moon

  bool waning = (phase > 0.5f);
  float terminatorX = r * cos(phase * 2.0f * PI);

  for (int dy = -r; dy <= r; dy++) {
    int py = cy + dy;
    if (py < 0 || py >= 240) continue;
    float chordW = sqrt(max(0.0f, (float)(r * r - dy * dy)));
    float termW  = fabs(terminatorX) *
                   sqrt(max(0.0f, 1.0f - ((float)(dy * dy) / (float)(r * r))));

    if (!waning) {
      if (terminatorX > 0) {
        tft.drawFastHLine(cx - (int)chordW, py,
          constrain((int)chordW + (int)termW, 0, 2 * r), COL_SHADOW);
      } else {
        tft.drawFastHLine(cx - (int)chordW, py,
          constrain((int)chordW - (int)termW, 0, 2 * r), COL_SHADOW);
      }
    } else {
      if (terminatorX < 0) {
        int shadowStart = cx + (int)termW;
        int shadowLen   = (int)chordW - (int)termW;
        if (shadowLen > 0)
          tft.drawFastHLine(shadowStart, py, shadowLen, COL_SHADOW);
      } else {
        tft.drawFastHLine(cx - (int)chordW, py,
          constrain((int)chordW - (int)termW, 0, 2 * r), COL_SHADOW);
      }
    }
  }

  // Soft terminator edge
  for (int dy = -r; dy <= r; dy++) {
    int py = cy + dy;
    if (py < 0 || py >= 240) continue;
    float termW = fabs(terminatorX) *
                  sqrt(max(0.0f, 1.0f - ((float)(dy * dy) / (float)(r * r))));
    int tx = waning ? cx + (int)termW : cx - (int)termW;
    if (tx > cx - r && tx < cx + r) {
      tft.drawPixel(tx,     py, 0x4A69);
      tft.drawPixel(tx + 1, py, 0x4A69);
    }
  }
}

// ─── MAIN MOON DRAW ───────────────────────────────────────────
void drawMoon() {
  float phase = getLunarPhase();

  // Update libration every 10 seconds
  if (millis() - lastLibUpdate > 10000) {
    librationAngle += 0.05f;
    if (librationAngle > TWO_PI) librationAngle = 0;
    lastLibUpdate = millis();
  }

  int libX = (int)(5.0f * sin(librationAngle));
  int libY = (int)(3.0f * sin(librationAngle * 0.7f));

  // Only fully redraw if phase changed meaningfully or libration shifted
  bool phaseChanged = fabs(phase - lastMoonPhase) > 0.001f;
  bool libChanged   = (libX != (int)(5.0f * sin(lastLibAngle)));
  bool firstDraw    = (lastMoonPhase < 0);

  if (!firstDraw && !phaseChanged && !libChanged) return;

  lastMoonPhase = phase;
  lastLibAngle  = librationAngle;
  lastMoonDraw  = millis();

  // Clear screen
  tft.fillScreen(COL_SPACE);

  // Stars
  generateStars();
  drawStars();

  // Moon
  drawMoonSurface(MOON_X, MOON_Y, MOON_R, libX, libY);
  drawPhaseShadow(MOON_X, MOON_Y, MOON_R, phase);

  // Moon outline
  tft.drawCircle(MOON_X, MOON_Y, MOON_R, 0x6B4D);

  // Phase name
  tft.setTextColor(COL_SUBTEXT, COL_SPACE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.drawString(getPhaseName(phase), 160, 218);

  // Date bottom right
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char dateBuf[20];
    strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", &timeinfo);
    tft.setTextColor(0x4A69, COL_SPACE);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(dateBuf, 315, 218);
  }

  Serial.printf("Moon: phase=%.3f (%s) libX=%d libY=%d\n",
    phase, getPhaseName(phase).c_str(), libX, libY);
}

// ═══════════════════════════════════════════════════════════════
//  TOUCH HANDLING
// ═══════════════════════════════════════════════════════════════
void handleTouch() {
  if (!touch.tirqTouched() || !touch.touched()) return;

  TS_Point p = touch.getPoint();
  int x = map(p.x, 200, 3800, 0, 320);
  int y = map(p.y, 200, 3800, 0, 240);

  Serial.printf("Touch x=%d y=%d\n", x, y);

  // Only process playback touches when Spotify is active
  if (lastTrack.isEmpty()) return;

  // Volume bar
  if (y > VOL_Y1 - 10 && y < VOL_Y2 + 10 && x > VOL_X1 && x < VOL_X2) {
    int tappedVolume = map(x, VOL_X1, VOL_X2, 0, 100);
    adjustVolume(tappedVolume);
  }
  // Playback buttons
  else if (y > BTN_Y1 && y < BTN_Y2) {
    if (x > BTN_PREV_X1 && x < BTN_PREV_X2) {
      drawStatusMessage("Previous...");
      spotify.previousTrack();
      delay(500);
    }
    else if (x > BTN_PLAY_X1 && x < BTN_PLAY_X2) {
      if (isPlaying) { spotify.pause(); isPlaying = false; }
      else           { spotify.play();  isPlaying = true;  }
      drawButtons(isPlaying);
    }
    else if (x > BTN_NEXT_X1 && x < BTN_NEXT_X2) {
      drawStatusMessage("Skipping...");
      spotify.nextTrack();
      delay(500);
    }
  }
  delay(300);
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Display
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  // Touch
  touchSPI.begin(25, 39, 32, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  // Splash screen
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.drawString("SPOTIFY", 160, 90);
  tft.setTextFont(2);
  tft.setTextColor(COL_SUBTEXT, COL_BG);
  tft.drawString("Connecting to WiFi...", 160, 130);

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  tft.fillRect(0, 115, 320, 30, COL_BG);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.drawString("Connected!", 160, 130);
  delay(800);

  // NTP — UTC+8 Singapore
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // Wait for NTP sync
  Serial.print("Syncing time");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced");

  // Spotify
  client.setInsecure();

  // Show moon while waiting for first Spotify poll
  drawMoon();
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  handleTouch();

  // Smoothly advance progress bar between polls
  if (isPlaying && trackDurationMs > 0) {
    unsigned long elapsed = millis() - lastProgressSync;
    trackProgressMs = constrain(
      trackProgressMs + (int)elapsed, 0, trackDurationMs);
    lastProgressSync = millis();
    drawProgressBar();
  }

  // Poll Spotify every 3 seconds
  if (millis() - lastPoll > POLL_INTERVAL) {
    lastPoll = millis();
    int status = spotify.getCurrentlyPlaying(
      trackCallback, SPOTIFY_NUM_ARTISTS(1));

    if (status == 204) {
      // Nothing playing — reset track state and show moon
      if (!lastTrack.isEmpty()) {
        // Track just stopped — clear state and switch to moon
        lastTrack       = "";
        lastArtist      = "";
        lastAlbumArtUrl = "";
        isPlaying       = false;
        lastMoonPhase   = -1.0; // force moon redraw
      }
      drawMoon();
    } else if (status != 200) {
      Serial.printf("Spotify error: %d\n", status);
    }
  }

  // Update moon libration every 10 seconds even without a poll
  if (lastTrack.isEmpty() && millis() - lastLibUpdate > 10000) {
    drawMoon();
  }
}