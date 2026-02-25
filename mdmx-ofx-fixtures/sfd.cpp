/*
 * Copyright (c) 2017 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "sfd.h"

#pragma comment(lib, "comdlg32.lib")


static const wchar_t *last_error;

const wchar_t* sfd_get_error(void) {
  const wchar_t*res = last_error;
  last_error = NULL;
  return res;
}


static int next_filter(wchar_t *dst, const wchar_t **p) {
  int len;

  *p += wcsspn(*p, L"|");
  if (**p == '\0') {
    return 0;
  }

  len = wcscspn(*p, L"|");
  memcpy(dst, *p, len*sizeof(wchar_t));
  dst[len] = '\0';
  *p += len;

  return 1;
}


/******************************************************************************
** Windows
*******************************************************************************/

#ifdef _WIN32

#include <windows.h>

struct FindMainWindowInfo {
  unsigned long process_id;
  HWND handle_root;
  HWND handle_first;
};


static int find_main_window_callback(HWND handle, LPARAM lParam) {
  FindMainWindowInfo* info = (FindMainWindowInfo*)lParam;
  unsigned long process_id = 0;
  GetWindowThreadProcessId(handle, &process_id);
  if (info->process_id == process_id) {
    info->handle_first = handle;
    if (GetWindow(handle, GW_OWNER) == 0 && IsWindowVisible(handle)) {
      info->handle_root = handle;
      return 0;
    }
  }
  return 1;
}


static HWND find_main_window() {
  FindMainWindowInfo info = {};
  info.process_id = GetCurrentProcessId();

  EnumWindows(find_main_window_callback, (LPARAM)&info);
  return info.handle_root;
}


static const wchar_t* make_filter_str(sfd_Options *opt) {
  static wchar_t buf[1024];
  int n;

  buf[0] = '\0';
  n = 0;

  if (opt->filter) {
    const wchar_t*p;
    wchar_t b[32];
    const wchar_t*name = opt->filter_name ? opt->filter_name : opt->filter;
    n += swprintf(buf + n, sizeof(buf)-n, L"%s", name) + 1;

    p = opt->filter;
    while (next_filter(b, &p)) {
      n += swprintf(buf + n, sizeof(buf) - n, L"%s;", b);
    }

    buf[++n] = '\0';
  }

  n += swprintf(buf + n, sizeof(buf) - n, L"All Files") + 1;
  n += swprintf(buf + n, sizeof(buf) - n, L"*.*");
  buf[++n] = '\0';

  return buf;
}


static void init_ofn(OPENFILENAME *ofn, sfd_Options *opt) {
  static wchar_t result_buf[2048];
  result_buf[0] = '\0';

  memset(ofn, 0, sizeof(*ofn));
  ofn->hwndOwner        = find_main_window();
  ofn->lStructSize      = sizeof(*ofn);
  ofn->lpstrFilter      = make_filter_str(opt);
  ofn->nFilterIndex     = 1;
  ofn->lpstrFile        = result_buf;
  ofn->Flags            = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
  ofn->nMaxFile         = sizeof(result_buf) - 1;
  ofn->lpstrInitialDir  = opt->path;
  ofn->lpstrTitle       = opt->title;
  ofn->lpstrDefExt      = opt->extension;
}


const wchar_t* sfd_open_dialog(sfd_Options *opt) {
  // NOTE(valuef): OpenFileName will change the CWD of our whole app so we have to keep track of that.
  // 2026-01-16
  auto req_size = GetCurrentDirectoryW(0, 0) * sizeof(wchar_t);
  LPWSTR old_cwd = 0;

  if (req_size > 0) {
    old_cwd = (LPWSTR)malloc(req_size);
  }

  if (old_cwd) {
    GetCurrentDirectoryW(req_size, old_cwd);
  }

  OPENFILENAME ofn;
  last_error = NULL;
  init_ofn(&ofn, opt);
  int ok = GetOpenFileName(&ofn);

  if (old_cwd) {
    SetCurrentDirectory(old_cwd);
    free(old_cwd);
  }

  if(!ok) {
    return 0;
  }

  return ofn.lpstrFile;
}

const wchar_t* sfd_save_dialog(sfd_Options *opt) {
  int ok;
  OPENFILENAME ofn;
  last_error = NULL;
  init_ofn(&ofn, opt);
  // TODO does this also change the CWD? Don't care right now
  // 2026-01-16
  ok = GetSaveFileName(&ofn);
  return ok ? ofn.lpstrFile : NULL;
}

#endif
