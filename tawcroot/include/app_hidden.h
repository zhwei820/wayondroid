/* Paths a guest's apps must not see: <store>/app-hidden, one guest-
 * absolute path per line, written by the host before init starts (the
 * rootless engine lists Waydroid's and LineageOS's own files there, which
 * a phone lacks and risk SDKs probe for). For an app process (uid
 * >= 10000) a listed path, or anything under a listed directory, does not
 * exist: path translation fails with ENOENT and directory listings leave
 * it out. Every other process sees the files, so the services that need
 * them run as before. Matching is on the path as given, before symlinks:
 * the list spells each file every way the image reaches it. */
#pragma once

/* 1 when `path` (guest-absolute) is hidden from the calling process */
int tawcroot_app_hidden(const char *path);

/* Drops the hidden entries of guest directory `dir` from a getdents64
 * buffer of `n` bytes; returns the length kept */
long tawcroot_app_hidden_filter(const char *dir, char *buf, long n);

/* 1 when the calling guest process is an app (uid >= 10000) */
int tawcroot_guest_is_app(void);
