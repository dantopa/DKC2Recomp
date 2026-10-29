#include "third_party/tinyfiledialogs.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Horizon has no desktop file dialogs. Every picker reports a cancel, so the
 * launcher and overlay keep their configured paths. */

int tinyfd_messageBox(char const *title, char const *message,
                      char const *dialog_type, char const *icon_type,
                      int default_button) {
  (void)dialog_type;
  (void)icon_type;
  fprintf(stderr, "%s: %s\n", title ? title : "DKC2Recomp",
          message ? message : "");
  return default_button;
}

char *tinyfd_saveFileDialog(char const *title, char const *default_path,
                            int filter_count, char const *const *filters,
                            char const *filter_description) {
  (void)title;
  (void)default_path;
  (void)filter_count;
  (void)filters;
  (void)filter_description;
  return NULL;
}

char *tinyfd_openFileDialog(char const *title, char const *default_path,
                            int filter_count, char const *const *filters,
                            char const *filter_description,
                            int allow_multiple) {
  (void)title;
  (void)default_path;
  (void)filter_count;
  (void)filters;
  (void)filter_description;
  (void)allow_multiple;
  return NULL;
}

char *tinyfd_selectFolderDialog(char const *title, char const *default_path) {
  (void)title;
  (void)default_path;
  return NULL;
}

/* Mesa's static GLES library exports no compatibility-profile entry points,
 * although the presenter's GL 2.1 context implements them. Each is resolved
 * from the current context on first use. */
static void LoadCompatibilityProc(const char *name, void *out, size_t size) {
  void *proc = SDL_GL_GetProcAddress(name);
  if (!proc || size != sizeof proc) {
    fprintf(stderr, "fatal: OpenGL entry point %s is unavailable\n", name);
    abort();
  }
  memcpy(out, &proc, size);
}

void APIENTRY glBegin(GLenum mode) {
  static void(APIENTRY *proc)(GLenum);
  if (!proc) LoadCompatibilityProc("glBegin", &proc, sizeof proc);
  proc(mode);
}

void APIENTRY glEnd(void) {
  static void(APIENTRY *proc)(void);
  if (!proc) LoadCompatibilityProc("glEnd", &proc, sizeof proc);
  proc();
}

void APIENTRY glVertex2f(GLfloat x, GLfloat y) {
  static void(APIENTRY *proc)(GLfloat, GLfloat);
  if (!proc) LoadCompatibilityProc("glVertex2f", &proc, sizeof proc);
  proc(x, y);
}

void APIENTRY glTexCoord2f(GLfloat s, GLfloat t) {
  static void(APIENTRY *proc)(GLfloat, GLfloat);
  if (!proc) LoadCompatibilityProc("glTexCoord2f", &proc, sizeof proc);
  proc(s, t);
}

void APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue,
                        GLfloat alpha) {
  static void(APIENTRY *proc)(GLfloat, GLfloat, GLfloat, GLfloat);
  if (!proc) LoadCompatibilityProc("glColor4f", &proc, sizeof proc);
  proc(red, green, blue, alpha);
}

void APIENTRY glMatrixMode(GLenum mode) {
  static void(APIENTRY *proc)(GLenum);
  if (!proc) LoadCompatibilityProc("glMatrixMode", &proc, sizeof proc);
  proc(mode);
}

void APIENTRY glLoadIdentity(void) {
  static void(APIENTRY *proc)(void);
  if (!proc) LoadCompatibilityProc("glLoadIdentity", &proc, sizeof proc);
  proc();
}
