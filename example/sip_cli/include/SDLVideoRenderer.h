#pragma once

#include <string>
#include <SDL3/SDL.h>

#include "manager.h"
#include "video/video_scaler.h"

class SDLVideoRenderer {
public:
    SDLVideoRenderer(const std::string& id, int width, int height);
    ~SDLVideoRenderer();

    bool init();
    void update(libsip_core::FrameBuffer& frame);
    void render();

private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    SDL_Mutex* m_mtxFrame = nullptr;
    int m_width, m_height;
    std::string m_id;

    AVFrame* m_frame;
    sip_core::video::VideoScaler m_scaler;
};
