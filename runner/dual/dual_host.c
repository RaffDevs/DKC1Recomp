/* Split-screen compositor: two independent DKC sessions, one window.
 *
 * The runner is a single-machine design (process-global g_snes/g_ppu), so two
 * cameras require two processes. Each child is the headless host in
 * --split-server mode; this host sends one input mask per frame to both,
 * waits for both framebuffers, and presents them top/bottom.
 *
 * POSIX only for the first milestone (macOS/Linux).
 */
#include <SDL.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#define MASK_B 0x001u
#define MASK_Y 0x002u
#define MASK_SELECT 0x004u
#define MASK_START 0x008u
#define MASK_UP 0x010u
#define MASK_DOWN 0x020u
#define MASK_LEFT 0x040u
#define MASK_RIGHT 0x080u
#define MASK_A 0x100u
#define MASK_X 0x200u
#define MASK_L 0x400u
#define MASK_R 0x800u

static volatile sig_atomic_t s_stop = 0;
static void OnSignal(int signo) { (void)signo; s_stop = 1; }

typedef struct {
  int fd;
  pid_t pid;
  int width;
  int height;
  int bpp;
  uint8_t *frame;
} Session;

static void Usage(const char *program) {
  fprintf(stderr,
          "usage: %s --rom <path.sfc> [--headless <path>] "
          "[--startup <masks.txt>] [--scale N] [--port-base N]\n",
          program);
}

static int ConnectWithRetry(int port, int attempts) {
  for (int attempt = 0; attempt < attempts; attempt++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in address;
      memset(&address, 0, sizeof address);
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons((uint16_t)port);
      if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0)
        return fd;
      close(fd);
    }
    struct timespec delay = {0, 100000000L}; /* 100 ms */
    nanosleep(&delay, NULL);
  }
  return -1;
}

static int RecvAll(int fd, uint8_t *data, size_t length) {
  size_t received = 0;
  while (received < length) {
    ssize_t n = recv(fd, data + received, length - received, 0);
    if (n <= 0) return 0;
    received += (size_t)n;
  }
  return 1;
}

static int SendAll(int fd, const uint8_t *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t n = send(fd, data + sent, length - sent, 0);
    if (n <= 0) return 0;
    sent += (size_t)n;
  }
  return 1;
}

static uint32_t PadMask(SDL_GameController *pad) {
  if (!pad || !SDL_GameControllerGetAttached(pad)) return 0;
  uint32_t mask = 0;
  const SDL_GameControllerButton map[12] = {
      SDL_CONTROLLER_BUTTON_DPAD_UP,       SDL_CONTROLLER_BUTTON_DPAD_DOWN,
      SDL_CONTROLLER_BUTTON_DPAD_LEFT,     SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
      SDL_CONTROLLER_BUTTON_A,             SDL_CONTROLLER_BUTTON_B,
      SDL_CONTROLLER_BUTTON_X,             SDL_CONTROLLER_BUTTON_Y,
      SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
      SDL_CONTROLLER_BUTTON_START,         SDL_CONTROLLER_BUTTON_BACK};
  const uint32_t bits[12] = {MASK_UP,   MASK_DOWN, MASK_LEFT,  MASK_RIGHT,
                             MASK_A,    MASK_B,    MASK_X,     MASK_Y,
                             MASK_L,    MASK_R,    MASK_START, MASK_SELECT};
  for (int i = 0; i < 12; i++) {
    if (SDL_GameControllerGetButton(pad, map[i])) mask |= bits[i];
  }
  /* Triggers double as L/R for controllers without shoulder digital read. */
  if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
    mask |= MASK_L;
  if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
    mask |= MASK_R;
  return mask;
}

static uint32_t KeyboardMask(const uint8_t *keys, int player) {
  uint32_t mask = 0;
  if (player == 0) {
    if (keys[SDL_SCANCODE_UP]) mask |= MASK_UP;
    if (keys[SDL_SCANCODE_DOWN]) mask |= MASK_DOWN;
    if (keys[SDL_SCANCODE_LEFT]) mask |= MASK_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) mask |= MASK_RIGHT;
    if (keys[SDL_SCANCODE_S]) mask |= MASK_A;
    if (keys[SDL_SCANCODE_Z]) mask |= MASK_B;
    if (keys[SDL_SCANCODE_A]) mask |= MASK_X;
    if (keys[SDL_SCANCODE_X]) mask |= MASK_Y;
    if (keys[SDL_SCANCODE_Q]) mask |= MASK_L;
    if (keys[SDL_SCANCODE_W]) mask |= MASK_R;
    if (keys[SDL_SCANCODE_RETURN]) mask |= MASK_START;
    if (keys[SDL_SCANCODE_RSHIFT]) mask |= MASK_SELECT;
  } else {
    if (keys[SDL_SCANCODE_I]) mask |= MASK_UP;
    if (keys[SDL_SCANCODE_K]) mask |= MASK_DOWN;
    if (keys[SDL_SCANCODE_J]) mask |= MASK_LEFT;
    if (keys[SDL_SCANCODE_L]) mask |= MASK_RIGHT;
    if (keys[SDL_SCANCODE_O]) mask |= MASK_A;
    if (keys[SDL_SCANCODE_U]) mask |= MASK_B;
    if (keys[SDL_SCANCODE_P]) mask |= MASK_X;
    if (keys[SDL_SCANCODE_SEMICOLON]) mask |= MASK_Y;
    if (keys[SDL_SCANCODE_9]) mask |= MASK_L;
    if (keys[SDL_SCANCODE_0]) mask |= MASK_R;
    if (keys[SDL_SCANCODE_BACKSLASH]) mask |= MASK_START;
    if (keys[SDL_SCANCODE_RCTRL]) mask |= MASK_SELECT;
  }
  return mask;
}

/* Optional deterministic launch route: one input mask per line (hex, snes9x
 * bit order), optionally "MASK * REPEAT". The same mask stream is sent to both
 * sessions until it ends, so both reach the same mode and level. After that,
 * live input takes over. */
static int LoadStartup(const char *path, uint32_t **out, size_t *count) {
  FILE *stream = fopen(path, "r");
  if (!stream) return 0;
  size_t capacity = 1024;
  uint32_t *masks = malloc(capacity * sizeof *masks);
  if (!masks) {
    fclose(stream);
    return 0;
  }
  *count = 0;
  char line[256];
  while (fgets(line, sizeof line, stream)) {
    char *comment = strpbrk(line, "#;");
    if (comment) *comment = '\0';
    char *text = line;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
      text++;
    if (!*text) continue;
    char *end = NULL;
    errno = 0;
    unsigned long mask = strtoul(text, &end, 16);
    if (errno || end == text) continue;
    while (*end == ' ' || *end == '\t') end++;
    unsigned long repeat = 1;
    if (*end == '*') {
      end++;
      repeat = strtoul(end, &end, 10);
      if (!repeat || repeat > 1000000ul) repeat = 1;
    }
    if (*count + repeat > capacity) {
      while (*count + repeat > capacity) capacity *= 2;
      uint32_t *grown = realloc(masks, capacity * sizeof *masks);
      if (!grown) {
        free(masks);
        fclose(stream);
        return 0;
      }
      masks = grown;
    }
    for (unsigned long i = 0; i < repeat; i++) masks[(*count)++] = (uint32_t)mask;
  }
  fclose(stream);
  *out = masks;
  return 1;
}

static int Shutdown(Session *session) {
  if (session->fd >= 0) {
    const uint8_t shutdown_mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    (void)SendAll(session->fd, shutdown_mask, sizeof shutdown_mask);
    close(session->fd);
    session->fd = -1;
  }
  if (session->pid > 0) {
    int status = 0;
    pid_t result = waitpid(session->pid, &status, WNOHANG);
    if (result == 0) {
      struct timespec delay = {0, 200000000L};
      nanosleep(&delay, NULL);
      result = waitpid(session->pid, &status, WNOHANG);
    }
    if (result == 0) {
      kill(session->pid, SIGTERM);
      waitpid(session->pid, &status, 0);
    }
    session->pid = -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *rom = NULL;
  const char *headless = "./dkc1_snesrecomp_headless";
  const char *startup_path = NULL;
  uint32_t *startup = NULL;
  size_t startup_count = 0;
  int scale = 2;
  int port_base = 27410;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(argv[i], "--headless") && i + 1 < argc)
      headless = argv[++i];
    else if (!strcmp(argv[i], "--startup") && i + 1 < argc)
      startup_path = argv[++i];
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
      scale = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--port-base") && i + 1 < argc)
      port_base = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--help")) {
      Usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      Usage(argv[0]);
      return 2;
    }
  }
  if (!rom || scale < 1 || scale > 6) {
    Usage(argv[0]);
    return 2;
  }
  if (startup_path && !LoadStartup(startup_path, &startup, &startup_count)) {
    fprintf(stderr, "dual host: unable to load startup route %s\n",
            startup_path);
    return 2;
  }
  if (startup_count)
    fprintf(stderr, "dual host: startup route %zu frames\n", startup_count);

  Session sessions[2];
  memset(sessions, 0, sizeof sessions);
  for (int i = 0; i < 2; i++) {
    sessions[i].fd = -1;
    sessions[i].pid = -1;
    pid_t pid = fork();
    if (pid == 0) {
      char port_text[16];
      snprintf(port_text, sizeof port_text, "%d", port_base + i);
      execl(headless, headless, "--split-server", port_text, rom,
            (char *)NULL);
      fprintf(stderr, "dual host: execl %s failed: %s\n", headless,
              strerror(errno));
      _exit(127);
    }
    sessions[i].pid = pid;
  }
  for (int i = 0; i < 2; i++) {
    sessions[i].fd = ConnectWithRetry(port_base + i, 100);
    if (sessions[i].fd < 0) {
      fprintf(stderr, "dual host: session %d did not start\n", i);
      Shutdown(&sessions[0]);
      Shutdown(&sessions[1]);
      return 3;
    }
    uint8_t header[12];
    if (!RecvAll(sessions[i].fd, header, sizeof header) ||
        memcmp(header, "DKC1", 4) != 0) {
      fprintf(stderr, "dual host: bad session %d header\n", i);
      Shutdown(&sessions[0]);
      Shutdown(&sessions[1]);
      return 4;
    }
    sessions[i].width = header[4] | (header[5] << 8);
    sessions[i].height = header[6] | (header[7] << 8);
    sessions[i].bpp = header[8];
    if (sessions[i].width != 256 || sessions[i].height != 224 ||
        sessions[i].bpp != 4) {
      fprintf(stderr, "dual host: unexpected session geometry %dx%d bpp=%d\n",
              sessions[i].width, sessions[i].height, sessions[i].bpp);
      Shutdown(&sessions[0]);
      Shutdown(&sessions[1]);
      return 4;
    }
    sessions[i].frame = malloc((size_t)sessions[i].width *
                               (size_t)sessions[i].height * 4u);
    if (!sessions[i].frame) {
      Shutdown(&sessions[0]);
      Shutdown(&sessions[1]);
      return 5;
    }
  }

  int width = sessions[0].width * scale;
  int height = sessions[0].height * scale;
  const int divider = 4;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
               SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "dual host: SDL_Init: %s\n", SDL_GetError());
    Shutdown(&sessions[0]);
    Shutdown(&sessions[1]);
    return 6;
  }
  SDL_Window *window = SDL_CreateWindow(
      "DKC1 Split (2P Team)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      width * 2 + divider, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!window || !renderer) {
    fprintf(stderr, "dual host: SDL window/renderer: %s\n", SDL_GetError());
    Shutdown(&sessions[0]);
    Shutdown(&sessions[1]);
    return 7;
  }
  SDL_Texture *textures[2];
  for (int i = 0; i < 2; i++) {
    textures[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    sessions[i].width, sessions[i].height);
    if (!textures[i]) {
      fprintf(stderr, "dual host: SDL texture: %s\n", SDL_GetError());
      Shutdown(&sessions[0]);
      Shutdown(&sessions[1]);
      return 8;
    }
  }

  SDL_GameController *pads[2] = {NULL, NULL};
  for (int i = 0; i < SDL_NumJoysticks() && i < 2; i++) {
    if (SDL_IsGameController(i)) pads[i] = SDL_GameControllerOpen(i);
  }

  signal(SIGTERM, OnSignal);
  signal(SIGINT, OnSignal);
  int running = 1;
  size_t startup_frame = 0;
  while (running && !s_stop) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = 0;
      if (event.type == SDL_KEYDOWN &&
          event.key.keysym.sym == SDLK_ESCAPE)
        running = 0;
      if (event.type == SDL_CONTROLLERDEVICEADDED &&
          event.cdevice.which < 2 && !pads[event.cdevice.which] &&
          SDL_IsGameController(event.cdevice.which))
        pads[event.cdevice.which] =
            SDL_GameControllerOpen(event.cdevice.which);
      if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int i = 0; i < 2; i++) {
          if (pads[i] &&
              SDL_GameControllerGetJoystick(pads[i]) ==
                  SDL_JoystickFromInstanceID(event.cdevice.which)) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
          }
        }
      }
    }
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    uint32_t masks[2] = {KeyboardMask(keys, 0) | PadMask(pads[0]),
                         KeyboardMask(keys, 1) | PadMask(pads[1])};
    if (startup_frame < startup_count) {
      masks[0] = masks[1] = startup[startup_frame++];
    }

    int ok = 1;
    for (int i = 0; i < 2 && ok; i++) {
      uint8_t bytes[4] = {(uint8_t)masks[i], (uint8_t)(masks[i] >> 8),
                          (uint8_t)(masks[i] >> 16),
                          (uint8_t)(masks[i] >> 24)};
      ok = SendAll(sessions[i].fd, bytes, sizeof bytes);
      if (ok)
        ok = RecvAll(sessions[i].fd, sessions[i].frame,
                     (size_t)sessions[i].width * (size_t)sessions[i].height *
                         4u);
    }
    if (!ok) {
      fprintf(stderr, "dual host: a session disconnected\n");
      break;
    }

    for (int i = 0; i < 2; i++) {
      SDL_UpdateTexture(textures[i], NULL, sessions[i].frame,
                        sessions[i].width * 4);
    }
    SDL_RenderClear(renderer);
    for (int i = 0; i < 2; i++) {
      SDL_Rect rect = {i ? width + divider : 0, 0, width, height};
      SDL_RenderCopy(renderer, textures[i], NULL, &rect);
    }
    SDL_SetRenderDrawColor(renderer, 32, 32, 32, 255);
    SDL_Rect line = {width, 0, divider, height};
    SDL_RenderFillRect(renderer, &line);
    SDL_RenderPresent(renderer);
  }

  for (int i = 0; i < 2; i++) SDL_GameControllerClose(pads[i]);
  for (int i = 0; i < 2; i++) {
    SDL_DestroyTexture(textures[i]);
    free(sessions[i].frame);
    Shutdown(&sessions[i]);
  }
  free(startup);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  fprintf(stderr, "dual host: stopped\n");
  return 0;
}
