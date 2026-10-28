#pragma once
#include <SDL3/SDL.h>
#include "nvn.h"

void custom_tex_init(void);
int custom_tex_count(void);
// Caller owns the returned RGBA32 surface; NULL leaves the original texture intact.
SDL_Surface *custom_tex_load(const NvnTexture *t);
// Background decoding: the replacement for the texture's current data (render thread), then its image once decoded
// (NULL and *pending while it is being decoded - ask again later; NULL without pending: none).
const void *custom_tex_match(const NvnTexture *t);
SDL_Surface *custom_tex_fetch(const void *match, bool *pending);
