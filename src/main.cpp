#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "glu32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define WINDOW_WIDTH   900
#define WINDOW_HEIGHT  700
#define PANEL_WIDTH    260
#define VIEWPORT_WIDTH (WINDOW_WIDTH - PANEL_WIDTH)
#define ID_AI_TIMER    1
#define AI_DELAY_MS    400

enum Player { NONE = 0, PLAYER_X = 1, PLAYER_O = 2 };
enum GameState { STATE_MODE_SELECT, STATE_SIDE_SELECT, STATE_PLAYING };
enum GameMode { MODE_VS_AI, MODE_TWO_PLAYER };

/* ---------- game state ---------- */
/* linear cell index = x + y*3 + z*9, x/y/z each in [0,2] */
int board[27];
int currentPlayer = PLAYER_X;
int humanPlayer = NONE;
int aiPlayer = NONE;
bool gameOver = false;
int winner = NONE;
bool isDraw = false;
GameState state = STATE_MODE_SELECT;
GameMode gameMode = MODE_VS_AI;

std::vector<std::array<int, 3>> winLines; /* each entry: 3 linear indices forming a line */

/* ---------- camera / rotation ---------- */
float rotationX = -20.0f; /* pitch */
float rotationY = 30.0f;  /* yaw   */
bool dragging = false;
int lastMouseX = 0, lastMouseY = 0;

/* ---------- OpenGL / Win32 handles ---------- */
HDC g_hdc = NULL;
HGLRC g_hglrc = NULL;
HWND g_hwnd = NULL;
GLuint g_fontListBase = 0;

/* ---------- fixed 2D UI buttons ---------- */
struct UIButton {
    RECT rect;
    int slot;            /* 0-26 for a board slot, -1 for an action button */
    const char* action;  /* "MODE_AI","MODE_2P","SIDE_X","SIDE_O","RESTART", or nullptr for slot buttons */
    const char* label;   /* display text for action buttons (slot buttons compute their own) */
};
std::vector<UIButton> uiButtons;

/* ---------- forward declarations ---------- */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void GenerateWinLines();
void ResetGame();
bool CheckWin(int player);
bool CheckDraw();
int  FindWinningMove(int player);
int  AiPickMove();
void UpdateStatus();
void StartAiTurn();
void BuildUIButtons();
void Render();

/* ================= game logic ================= */

/* enumerates all 49 winning lines of a 3x3x3 tic-tac-toe cube */
void GenerateWinLines() {
    winLines.clear();
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                /* only keep one direction from each +d/-d pair */
                bool canonical = (dx > 0) || (dx == 0 && dy > 0) || (dx == 0 && dy == 0 && dz > 0);
                if (!canonical) continue;

                for (int x = 0; x < 3; x++) {
                    for (int y = 0; y < 3; y++) {
                        for (int z = 0; z < 3; z++) {
                            int x2 = x + 2 * dx, y2 = y + 2 * dy, z2 = z + 2 * dz;
                            if (x2 < 0 || x2 > 2 || y2 < 0 || y2 > 2 || z2 < 0 || z2 > 2) continue;
                            int x1 = x + dx, y1 = y + dy, z1 = z + dz;
                            int i0 = x + y * 3 + z * 9;
                            int i1 = x1 + y1 * 3 + z1 * 9;
                            int i2 = x2 + y2 * 3 + z2 * 9;
                            winLines.push_back({ i0, i1, i2 });
                        }
                    }
                }
            }
        }
    }
}

void ResetGame() {
    for (int i = 0; i < 27; i++) board[i] = NONE;
    currentPlayer = PLAYER_X; /* X always moves first */
    gameOver = false;
    winner = NONE;
    isDraw = false;
}

bool CheckWin(int player) {
    for (auto& line : winLines) {
        if (board[line[0]] == player && board[line[1]] == player && board[line[2]] == player)
            return true;
    }
    return false;
}

bool CheckDraw() {
    for (int i = 0; i < 27; i++)
        if (board[i] == NONE) return false;
    return true;
}

/* returns a 0-26 cell index where `player` would win immediately, or -1 if none */
int FindWinningMove(int player) {
    for (int i = 0; i < 27; i++) {
        if (board[i] == NONE) {
            board[i] = player;
            bool wins = CheckWin(player);
            board[i] = NONE;
            if (wins) return i;
        }
    }
    return -1;
}

/* simple heuristic AI: win > block > center > corner > any remaining cell */
int AiPickMove() {
    int move = FindWinningMove(aiPlayer);
    if (move != -1) return move;

    move = FindWinningMove(humanPlayer);
    if (move != -1) return move;

    int center = 1 + 1 * 3 + 1 * 9;
    if (board[center] == NONE) return center;

    std::vector<int> corners;
    for (int x = 0; x <= 2; x += 2)
        for (int y = 0; y <= 2; y += 2)
            for (int z = 0; z <= 2; z += 2)
                corners.push_back(x + y * 3 + z * 9);

    int start = rand() % (int)corners.size();
    for (size_t i = 0; i < corners.size(); i++) {
        int idx = corners[(start + i) % corners.size()];
        if (board[idx] == NONE) return idx;
    }

    std::vector<int> empties;
    for (int i = 0; i < 27; i++) if (board[i] == NONE) empties.push_back(i);
    if (empties.empty()) return -1;
    return empties[rand() % empties.size()];
}

void UpdateStatus() {
    if (gameOver) return;
    char text[96];
    if (gameMode == MODE_TWO_PLAYER) {
        snprintf(text, sizeof(text), "Player %s's turn", currentPlayer == PLAYER_X ? "X" : "O");
    }
    else if (currentPlayer == humanPlayer) {
        snprintf(text, sizeof(text), "Your turn (%s)", humanPlayer == PLAYER_X ? "X" : "O");
    }
    else {
        snprintf(text, sizeof(text), "Computer is thinking...");
    }
    SetWindowTextA(g_hwnd, text);
}

void StartAiTurn() {
    currentPlayer = aiPlayer;
    SetWindowTextA(g_hwnd, "Computer is thinking...");
    SetTimer(g_hwnd, ID_AI_TIMER, AI_DELAY_MS, NULL);
}

/* ================= UI construction ================= */

void BuildUIButtons() {
    uiButtons.clear();
    int panelX = VIEWPORT_WIDTH;

    if (state == STATE_MODE_SELECT) {
        UIButton ai; ai.rect = { panelX + 15, 160, panelX + 245, 210 };
        ai.slot = -1; ai.action = "MODE_AI"; ai.label = "Play vs Computer";
        UIButton tp; tp.rect = { panelX + 15, 230, panelX + 245, 280 };
        tp.slot = -1; tp.action = "MODE_2P"; tp.label = "2 Player (local)";
        uiButtons.push_back(ai);
        uiButtons.push_back(tp);
    }
    else if (state == STATE_SIDE_SELECT) {
        UIButton bx; bx.rect = { panelX + 15, 160, panelX + 245, 210 };
        bx.slot = -1; bx.action = "SIDE_X"; bx.label = "Play as X";
        UIButton bo; bo.rect = { panelX + 15, 230, panelX + 245, 280 };
        bo.slot = -1; bo.action = "SIDE_O"; bo.label = "Play as O";
        uiButtons.push_back(bx);
        uiButtons.push_back(bo);
    }
    else {
        for (int layer = 0; layer < 3; layer++) {
            int top = 90 + layer * 150;
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 3; col++) {
                    int slot = col + row * 3 + layer * 9;
                    UIButton b;
                    b.rect.left = panelX + 15 + col * 48;
                    b.rect.top = top + 20 + row * 48;
                    b.rect.right = b.rect.left + 40;
                    b.rect.bottom = b.rect.top + 40;
                    b.slot = slot;
                    b.action = nullptr;
                    b.label = nullptr;
                    uiButtons.push_back(b);
                }
            }
        }

        UIButton r;
        r.rect = { panelX + 15, 610, panelX + 245, 650 };
        r.slot = -1;
        r.action = "RESTART";
        r.label = "Restart";
        uiButtons.push_back(r);
    }
}

/* ================= drawing ================= */

void DrawText2D(float x, float y, const char* text, float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos2f(x, y);
    glListBase(g_fontListBase);
    glCallLists((GLsizei)strlen(text), GL_UNSIGNED_BYTE, text);
}

void DrawWireCube(float cx, float cy, float cz, float half, float r, float g, float b) {
    float x0 = cx - half, x1 = cx + half;
    float y0 = cy - half, y1 = cy + half;
    float z0 = cz - half, z1 = cz + half;
    glColor3f(r, g, b);
    glBegin(GL_LINES);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0);
    glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0);
    glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    glVertex3f(x0, y1, z0); glVertex3f(x0, y0, z0);

    glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1);
    glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1);
    glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    glVertex3f(x0, y1, z1); glVertex3f(x0, y0, z1);

    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1);
    glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1);
    glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1);
    glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1);
    glEnd();
}

void DrawSolidCube(float cx, float cy, float cz, float half, float r, float g, float b) {
    float x0 = cx - half, x1 = cx + half;
    float y0 = cy - half, y1 = cy + half;
    float z0 = cz - half, z1 = cz + half;
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    glVertex3f(x1, y0, z0); glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0);
    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x0, y1, z0);
    glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1);
    glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1); glVertex3f(x0, y0, z1);
    glEnd();
}

void DrawSphere(float cx, float cy, float cz, float radius, float r, float g, float b) {
    glColor3f(r, g, b);
    glPushMatrix();
    glTranslatef(cx, cy, cz);
    GLUquadric* quad = gluNewQuadric();
    gluSphere(quad, radius, 16, 16);
    gluDeleteQuadric(quad);
    glPopMatrix();
}

/* places text at a 3D point using the CURRENT modelview/projection (so it moves with the
   rotating cube) while the glyphs themselves stay flat and readable, billboard-style */
void DrawLabel3D(float x, float y, float z, const char* text, float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos3f(x, y, z);
    glListBase(g_fontListBase);
    glCallLists((GLsizei)strlen(text), GL_UNSIGNED_BYTE, text);
}

void Render3DScene() {
    glViewport(0, 0, VIEWPORT_WIDTH, WINDOW_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)VIEWPORT_WIDTH / (double)WINDOW_HEIGHT, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 8, 0, 0, 0, 0, 1, 0);

    glEnable(GL_DEPTH_TEST);

    glRotatef(rotationX, 1, 0, 0);
    glRotatef(rotationY, 0, 1, 0);

    DrawWireCube(0, 0, 0, 1.5f, 0.55f, 0.55f, 0.6f); /* outer boundary */

    for (int x = 0; x < 3; x++) {
        for (int y = 0; y < 3; y++) {
            for (int z = 0; z < 3; z++) {
                int idx = x + y * 3 + z * 9;
                float cx = (x - 1) * 1.0f;
                float cy = (y - 1) * 1.0f;
                float cz = (z - 1) * 1.0f;
                DrawWireCube(cx, cy, cz, 0.35f, 0.75f, 0.75f, 0.78f);

                if (board[idx] == PLAYER_X) {
                    DrawSolidCube(cx, cy, cz, 0.22f, 0.86f, 0.24f, 0.24f);
                }
                else if (board[idx] == PLAYER_O) {
                    DrawSphere(cx, cy, cz, 0.24f, 0.24f, 0.4f, 0.86f);
                }

                /* number label near a corner of the slot, out of the way of the mark itself;
                   the offset is in local space so it rotates naturally along with the cube */
                char slotLabel[4];
                snprintf(slotLabel, sizeof(slotLabel), "%d", idx + 1);
                DrawLabel3D(cx + 0.1f, cy + 0.28f, cz + 0.28f, slotLabel, 0.95f, 0.85f, 0.3f);
            }
        }
    }
}

void DrawButton(const UIButton& b) {
    char label[24] = "";
    float bg_r = 0.85f, bg_g = 0.85f, bg_b = 0.88f;
    float text_r = 0.1f, text_g = 0.1f, text_b = 0.1f;

    if (b.action != nullptr) {
        strncpy(label, b.label, sizeof(label) - 1);
        if (strcmp(b.action, "SIDE_X") == 0) { bg_r = 0.86f; bg_g = 0.5f;  bg_b = 0.5f; }
        else if (strcmp(b.action, "SIDE_O") == 0) { bg_r = 0.5f;  bg_g = 0.6f;  bg_b = 0.86f; }
        else if (strcmp(b.action, "MODE_AI") == 0 || strcmp(b.action, "MODE_2P") == 0)
        {
            bg_r = 0.68f; bg_g = 0.78f; bg_b = 0.85f;
        }
        else { bg_r = 0.8f;  bg_g = 0.8f;  bg_b = 0.8f; }
    }
    else {
        int cell = board[b.slot];
        if (cell == PLAYER_X) {
            strcpy(label, "X");
            bg_r = 0.95f; bg_g = 0.8f; bg_b = 0.8f;
            text_r = 0.7f; text_g = 0.15f; text_b = 0.15f;
        }
        else if (cell == PLAYER_O) {
            strcpy(label, "O");
            bg_r = 0.8f; bg_g = 0.85f; bg_b = 0.95f;
            text_r = 0.15f; text_g = 0.25f; text_b = 0.7f;
        }
        else {
            snprintf(label, sizeof(label), "%d", b.slot + 1);
        }
    }

    glColor3f(bg_r, bg_g, bg_b);
    glBegin(GL_QUADS);
    glVertex2f((float)b.rect.left, (float)b.rect.top);
    glVertex2f((float)b.rect.right, (float)b.rect.top);
    glVertex2f((float)b.rect.right, (float)b.rect.bottom);
    glVertex2f((float)b.rect.left, (float)b.rect.bottom);
    glEnd();

    glColor3f(0.25f, 0.25f, 0.25f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)b.rect.left, (float)b.rect.top);
    glVertex2f((float)b.rect.right, (float)b.rect.top);
    glVertex2f((float)b.rect.right, (float)b.rect.bottom);
    glVertex2f((float)b.rect.left, (float)b.rect.bottom);
    glEnd();

    float textX = (float)b.rect.left + (b.action ? 10.0f : 14.0f);
    float textY = (float)(b.rect.top + b.rect.bottom) / 2.0f + 5.0f;
    DrawText2D(textX, textY, label, text_r, text_g, text_b);
}

void Render2DPanel() {
    glViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WINDOW_WIDTH, WINDOW_HEIGHT, 0, -1, 1); /* y-down, matches screen/mouse coords */

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    glColor3f(0.93f, 0.93f, 0.95f);
    glBegin(GL_QUADS);
    glVertex2f((float)VIEWPORT_WIDTH, 0);
    glVertex2f((float)WINDOW_WIDTH, 0);
    glVertex2f((float)WINDOW_WIDTH, (float)WINDOW_HEIGHT);
    glVertex2f((float)VIEWPORT_WIDTH, (float)WINDOW_HEIGHT);
    glEnd();

    glColor3f(0.5f, 0.5f, 0.5f);
    glBegin(GL_LINES);
    glVertex2f((float)VIEWPORT_WIDTH, 0);
    glVertex2f((float)VIEWPORT_WIDTH, (float)WINDOW_HEIGHT);
    glEnd();

    DrawText2D(VIEWPORT_WIDTH + 15.0f, 30.0f, "3D Tic Tac Toe", 0.1f, 0.1f, 0.1f);

    if (state == STATE_MODE_SELECT) {
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 130.0f, "Choose a mode:", 0.1f, 0.1f, 0.1f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 320.0f, "Drag the cube on the", 0.4f, 0.4f, 0.4f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 340.0f, "left to rotate it.", 0.4f, 0.4f, 0.4f);
    }
    else if (state == STATE_SIDE_SELECT) {
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 130.0f, "Choose your side:", 0.1f, 0.1f, 0.1f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 320.0f, "Drag the cube on the", 0.4f, 0.4f, 0.4f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 340.0f, "left to rotate it.", 0.4f, 0.4f, 0.4f);
    }
    else {
        char status[96];
        if (gameOver) {
            if (isDraw) snprintf(status, sizeof(status), "It's a draw!");
            else if (gameMode == MODE_TWO_PLAYER) snprintf(status, sizeof(status), "Player %s wins!", winner == PLAYER_X ? "X" : "O");
            else if (winner == humanPlayer) snprintf(status, sizeof(status), "You win!");
            else snprintf(status, sizeof(status), "Computer wins!");
        }
        else if (gameMode == MODE_TWO_PLAYER) {
            snprintf(status, sizeof(status), "Player %s's turn", currentPlayer == PLAYER_X ? "X" : "O");
        }
        else if (currentPlayer == humanPlayer) {
            snprintf(status, sizeof(status), "Your turn (%s)", humanPlayer == PLAYER_X ? "X" : "O");
        }
        else {
            snprintf(status, sizeof(status), "Computer is thinking...");
        }
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 55.0f, status, 0.1f, 0.1f, 0.55f);

        DrawText2D(VIEWPORT_WIDTH + 15.0f, 78.0f, "Layer 1 (back)", 0.3f, 0.3f, 0.3f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 78.0f + 150, "Layer 2 (middle)", 0.3f, 0.3f, 0.3f);
        DrawText2D(VIEWPORT_WIDTH + 15.0f, 78.0f + 300, "Layer 3 (front)", 0.3f, 0.3f, 0.3f);
    }

    for (auto& b : uiButtons) DrawButton(b);
}

void Render() {
    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Render3DScene();
    Render2DPanel();

    SwapBuffers(g_hdc);
}

/* ================= window procedure ================= */

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (x >= VIEWPORT_WIDTH) {
            for (auto& b : uiButtons) {
                if (x >= b.rect.left && x < b.rect.right && y >= b.rect.top && y < b.rect.bottom) {
                    if (b.action != nullptr) {
                        if (strcmp(b.action, "MODE_AI") == 0 && state == STATE_MODE_SELECT) {
                            gameMode = MODE_VS_AI;
                            state = STATE_SIDE_SELECT;
                            SetWindowTextA(hwnd, "3D Tic Tac Toe - choose a side");
                            BuildUIButtons();
                        }
                        else if (strcmp(b.action, "MODE_2P") == 0 && state == STATE_MODE_SELECT) {
                            gameMode = MODE_TWO_PLAYER;
                            humanPlayer = NONE; aiPlayer = NONE;
                            ResetGame(); state = STATE_PLAYING; BuildUIButtons();
                            UpdateStatus();
                        }
                        else if (strcmp(b.action, "SIDE_X") == 0 && state == STATE_SIDE_SELECT) {
                            humanPlayer = PLAYER_X; aiPlayer = PLAYER_O;
                            ResetGame(); state = STATE_PLAYING; BuildUIButtons();
                            if (currentPlayer == aiPlayer) StartAiTurn(); else UpdateStatus();
                        }
                        else if (strcmp(b.action, "SIDE_O") == 0 && state == STATE_SIDE_SELECT) {
                            humanPlayer = PLAYER_O; aiPlayer = PLAYER_X;
                            ResetGame(); state = STATE_PLAYING; BuildUIButtons();
                            if (currentPlayer == aiPlayer) StartAiTurn(); else UpdateStatus();
                        }
                        else if (strcmp(b.action, "RESTART") == 0) {
                            KillTimer(hwnd, ID_AI_TIMER);
                            state = STATE_MODE_SELECT;
                            SetWindowTextA(hwnd, "3D Tic Tac Toe - choose a mode");
                            BuildUIButtons();
                        }
                    }
                    else if (state == STATE_PLAYING && !gameOver &&
                        (gameMode == MODE_TWO_PLAYER || currentPlayer == humanPlayer)) {
                        int slot = b.slot;
                        if (board[slot] == NONE) {
                            int mover = currentPlayer;
                            board[slot] = mover;
                            if (CheckWin(mover)) {
                                gameOver = true; winner = mover;
                                if (gameMode == MODE_TWO_PLAYER) {
                                    char msg[64];
                                    snprintf(msg, sizeof(msg), "Player %s wins! Click Restart to play again", mover == PLAYER_X ? "X" : "O");
                                    SetWindowTextA(hwnd, msg);
                                }
                                else {
                                    SetWindowTextA(hwnd, "You win! Click Restart to play again");
                                }
                            }
                            else if (CheckDraw()) {
                                gameOver = true; isDraw = true;
                                SetWindowTextA(hwnd, "It's a draw! Click Restart to try again");
                            }
                            else if (gameMode == MODE_TWO_PLAYER) {
                                currentPlayer = (currentPlayer == PLAYER_X) ? PLAYER_O : PLAYER_X;
                                UpdateStatus();
                            }
                            else {
                                StartAiTurn();
                            }
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
            }
        }
        else {
            dragging = true;
            lastMouseX = x;
            lastMouseY = y;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (dragging) {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            rotationY += (x - lastMouseX) * 0.5f;
            rotationX += (y - lastMouseY) * 0.5f;
            lastMouseX = x;
            lastMouseY = y;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (dragging) {
            dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == 'R') {
            KillTimer(hwnd, ID_AI_TIMER);
            state = STATE_MODE_SELECT;
            SetWindowTextA(hwnd, "3D Tic Tac Toe - choose a mode");
            BuildUIButtons();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wParam == ID_AI_TIMER) {
            KillTimer(hwnd, ID_AI_TIMER);
            int move = AiPickMove();
            if (move >= 0) board[move] = aiPlayer;

            if (CheckWin(aiPlayer)) {
                gameOver = true; winner = aiPlayer;
                SetWindowTextA(hwnd, "Computer wins! Click Restart to try again");
            }
            else if (CheckDraw()) {
                gameOver = true; isDraw = true;
                SetWindowTextA(hwnd, "It's a draw! Click Restart to try again");
            }
            else {
                currentPlayer = humanPlayer;
                UpdateStatus();
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; /* we repaint everything ourselves */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        wglMakeCurrent(NULL, NULL);
        if (g_hglrc) wglDeleteContext(g_hglrc);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ================= entry point ================= */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;
    srand((unsigned int)time(NULL));
    GenerateWinLines();

    const char CLASS_NAME[] = "TicTacToe3DWindowClass";

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC; /* required for a persistent GL-capable device context */
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    RegisterClassA(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rect, style, FALSE);

    g_hwnd = CreateWindowExA(
        0, CLASS_NAME, "3D Tic Tac Toe - choose a mode",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);

    if (!g_hwnd) return 0;

    g_hdc = GetDC(g_hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (pf == 0 || !SetPixelFormat(g_hdc, pf, &pfd)) {
        MessageBoxA(NULL, "Failed to set pixel format", "Error", MB_OK);
        return 0;
    }

    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc || !wglMakeCurrent(g_hdc, g_hglrc)) {
        MessageBoxA(NULL, "Failed to create OpenGL context", "Error", MB_OK);
        return 0;
    }

    HFONT font = CreateFontA(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(g_hdc, font);
    g_fontListBase = glGenLists(128);
    wglUseFontBitmapsA(g_hdc, 0, 128, g_fontListBase);
    SelectObject(g_hdc, oldFont);
    DeleteObject(font);

    BuildUIButtons();

    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_fontListBase) glDeleteLists(g_fontListBase, 128);
    ReleaseDC(g_hwnd, g_hdc);

    return 0;
}