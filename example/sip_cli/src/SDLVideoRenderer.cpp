#include <SDL3/SDL_render.h>

#include "SDLVideoRenderer.h"

SDLVideoRenderer::SDLVideoRenderer(const std::string& id, int width, int height) : width_(width), height_(height) {
    SDL_CreateWindowAndRenderer(id.c_str(),
        width_, height_, SDL_EVENT_WINDOW_SHOWN,
        &window_, &renderer_);
    
    texture_ = SDL_CreateTexture(renderer_,
        SDL_PIXELFORMAT_YV12,  // Matches AV_PIX_FMT_YUV420P
        SDL_TEXTUREACCESS_STREAMING,
        width_, height_);
}

void SDLVideoRenderer::renderFrame(libsip_core::FrameBuffer& frame) {
    // Update SDL texture with YUV planes
    SDL_UpdateYUVTexture(texture_, nullptr,
        frame->data[0], frame->linesize[0],  // Y
        frame->data[1], frame->linesize[1],  // U
        frame->data[2], frame->linesize[2]); // V

    // Clear and present
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

SDLVideoRenderer::~SDLVideoRenderer() {
    SDL_DestroyTexture(texture_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
}