#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/vr/vr_fade.h"

int main(void) {
    float c[48] = {0}, rgba[4];
    const float vertices[4][3] = {{-406,-228,0},{406,-228,0},{406,228,0},{-406,228,0}};
    c[0]=2.0f/812; c[5]=-2.0f/456; c[10]=c[15]=c[16]=c[21]=c[26]=1;
    c[31]=0.75f;
    assert(vr_fade_quad_color(c,vertices,rgba));
    assert(rgba[0]==0 && rgba[3]==0.75f);
    c[0]*=0.5f; assert(!vr_fade_quad_color(c,vertices,rgba)); // Floating rectangle is not a full-view fade.
    c[0]*=2; c[31]=NAN; assert(!vr_fade_quad_color(c,vertices,rgba));
    c[31]=0.5f; c[15]=-1; assert(!vr_fade_quad_color(c,vertices,rgba)); c[15]=1;
    c[19]=500; assert(!vr_fade_quad_color(c,vertices,rgba)); c[19]=0;
    c[31]=1; c[28]=c[29]=c[30]=1; assert(vr_fade_quad_color(c,vertices,rgba)); // White and opaque endpoints.
    c[28]=0.2f; c[29]=0.4f; c[30]=0.7f; c[31]=0; assert(vr_fade_quad_color(c,vertices,rgba)); // Coloured fade endpoint.
    float out[4]={0}, black[4]={0,0,0,0.5f}, white[4]={1,1,1,0.5f};
    vr_fade_over(out,black); vr_fade_over(out,white);
    assert(out[0]==0.5f && out[1]==0.5f && out[2]==0.5f && out[3]==0.75f);
    float layer[4];
    const float nearly_black[4]={0,0,0,0.95f}, opaque[4]={0,0,0,1}, clear[4]={0};
    vr_fade_compositor_color(nearly_black,layer);
    assert(fabsf((1-layer[3])-vr_fade_srgb_to_linear(0.05f))<1e-7f);
    assert(layer[3]>0.996f); // 95% native fade must not leave 5% linear light (~25% display brightness).
    vr_fade_compositor_color(opaque,layer); assert(layer[3]==1);
    vr_fade_compositor_color(clear,layer); assert(layer[3]==0);
    vr_fade_compositor_color(white,layer); assert(layer[0]==0.5f && layer[3]==0.5f);
    float previous=0;
    for (int i=0;i<=255;i++) {
        float fade[4]={0,0,0,(float)i/255};
        vr_fade_compositor_color(fade,layer);
        assert(layer[3]>=previous && layer[3]<=1);
        previous=layer[3];
    }
    // Wide +/-80-degree rays from either eye intersect inside the head-locked veil.
    const float outer_ray = 0.04f + VR_FADE_OVERLAY_DISTANCE*tanf(80.0f*3.14159265f/180.0f);
    assert(outer_ray < VR_FADE_OVERLAY_SIZE/2);
    VrFadeFrame frame={0};
    float fill[4]={0,0,0,0};
    vr_fade_frame_fill(&frame,black,true);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==0.5f);
    vr_fade_frame_draw(&frame); vr_fade_frame_fill(&frame,fill,false);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==1); // Opaque black hold, even with native alpha zero.
    vr_fade_frame_draw(&frame); vr_fade_frame_fill(&frame,black,true);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==0.5f); // Fade back in, with no stuck black state.
    vr_fade_frame_fill(&frame,fill,false); vr_fade_frame_draw(&frame);
    assert(!vr_fade_frame_take(&frame,layer)); // Menu/text drawn over a background must stay visible.
    vr_fade_frame_fill(&frame,black,true); vr_fade_frame_fill(&frame,fill,false);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==1);
    assert(!vr_fade_frame_take(&frame,layer)); // Frame state never resurrects a preceding black hold.
    vr_fade_frame_fill(&frame,fill,false); vr_fade_frame_fill(&frame,black,true);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==1); // A blended draw cannot erase an opaque hold below it.
    vr_fade_frame_fill(&frame,fill,false); vr_fade_frame_fill(&frame,clear,true);
    assert(vr_fade_frame_take(&frame,layer) && layer[3]==1); // Transparent endpoint preserves native opaque coverage.
    vr_fade_frame_fill(&frame,fill,false); vr_fade_frame_fill(&frame,white,true);
    vr_fade_frame_draw(&frame);
    assert(!vr_fade_frame_take(&frame,layer)); // Text after a tinted opaque backdrop is still ordinary UI.
    puts("VR fade: coverage, blending, and near-black compositor conversion passed");
}
