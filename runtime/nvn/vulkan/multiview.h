// VR multiview shaders (multiview.c).
#pragma once

// The shader made to render both eyes (gl_ViewIndex): each constant buffer and texture binding an array of 2 (eye 0,
// eye 1), malloc'd; NULL when it can't be.
char *vk_multiview_glsl(const char *src);
