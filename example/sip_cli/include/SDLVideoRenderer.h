#pragma once

#include <string>
#include <SDL3/SDL.h>
#include "manager.h"

class SDLVideoRenderer {
public:
    SDLVideoRenderer(const std::string& id, int width, int height);
    ~SDLVideoRenderer();

    void renderFrame(libsip_core::FrameBuffer& frame);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_, height_;
};
