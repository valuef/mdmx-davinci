/**
 * Copyright (c) 2017 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `sfd.c` for details.
 */

#ifndef SFD_H
#define SFD_H

#define SFD_VERSION "0.1.0"

struct sfd_Options {
  const wchar_t* title;
  const wchar_t* path;
  const wchar_t* filter_name;
  const wchar_t* filter;
  const wchar_t* extension;
};

const wchar_t* sfd_get_error(void);
const wchar_t* sfd_open_dialog(sfd_Options * opt);
const wchar_t* sfd_save_dialog(sfd_Options * opt);

#endif
