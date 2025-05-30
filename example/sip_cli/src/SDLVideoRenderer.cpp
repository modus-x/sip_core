#include <SDL3/SDL_render.h>

#include "SDLVideoRenderer.h"

SDLVideoRenderer::SDLVideoRenderer(const std::string& id, int width, int height) : m_width(width), m_height(height), m_id(id), m_frame(av_frame_alloc())
{
}

SDLVideoRenderer::~SDLVideoRenderer()
{
    if(m_mtxFrame)
    SDL_LockMutex(m_mtxFrame);

    if(m_texture)
    SDL_DestroyTexture(m_texture);
    
    if(m_renderer)
    SDL_DestroyRenderer(m_renderer);
    
    if(m_window)
    SDL_DestroyWindow(m_window);
    
    if(m_mtxFrame)
    SDL_UnlockMutex(m_mtxFrame);

    if(m_mtxFrame)
    SDL_DestroyMutex(m_mtxFrame);
    
    av_frame_free(&m_frame);
    
}

bool SDLVideoRenderer::init()
{
    if(!SDL_CreateWindowAndRenderer(m_id.c_str(),
        m_width, m_height, SDL_EVENT_WINDOW_SHOWN,
        &m_window, &m_renderer))
    {
        return false;
    }
    
    m_texture = SDL_CreateTexture(m_renderer,
        SDL_PIXELFORMAT_RGB24,  // Matches AV_PIX_FMT_YUV420P
        SDL_TEXTUREACCESS_STREAMING,
        m_width, m_height);

    if(m_texture == nullptr) {
        SDL_DestroyRenderer(m_renderer);
        SDL_DestroyWindow(m_window);
        return false;
    }

    m_mtxFrame = SDL_CreateMutex();
    if (!m_mtxFrame) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create mutex\n");
        return false;
    }

    SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255); // White
    SDL_RenderClear(m_renderer);
    SDL_RenderPresent(m_renderer);

    return true;
}

void SDLVideoRenderer::update(libsip_core::FrameBuffer& frame) 
{
    if (frame->width == 0 || frame->height == 0)
        return;
        
    SDL_LockMutex(m_mtxFrame);

    av_frame_free(&m_frame);
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_RGB24;
    m_frame->width = frame->width;
    m_frame->height = frame->height;

    av_frame_get_buffer(m_frame, 32);
    m_scaler.scale(frame.get(), m_frame);
    av_frame_copy_props(m_frame, frame.get());
    
    SDL_UnlockMutex(m_mtxFrame);
}

void SDLVideoRenderer::render()
{
    
    void *dst_pixels;
    int dst_pitch;

    if (!SDL_LockTexture(m_texture, NULL, &dst_pixels, &dst_pitch)) {
        return;
    }
    
    SDL_LockMutex(m_mtxFrame);
    // Copy data from AVFrame to texture
    uint8_t *src_data = m_frame->data[0];
    if(!src_data || src_data[0] == '\0') {
        SDL_UnlockMutex(m_mtxFrame);
        SDL_UnlockTexture(m_texture);
        return;
    }
    
    bool same_size = (m_frame->width == m_width) && (m_frame->width == m_height);
    if(!same_size) {
        SDL_UnlockTexture(m_texture);
        
        if(!SDL_SetWindowSize(m_window, m_frame->width, m_frame->height)) {
            SDL_UnlockMutex(m_mtxFrame);
            return;
        }
        
        SDL_DestroyTexture(m_texture);
        m_texture = SDL_CreateTexture(m_renderer,
            SDL_PIXELFORMAT_RGB24,  // Matches AV_PIX_FMT_YUV420P
            SDL_TEXTUREACCESS_STREAMING,
            m_frame->width, m_frame->height);

        if(m_texture == nullptr) {
            SDL_UnlockMutex(m_mtxFrame);
            return;
        }

            
        m_width = m_frame->width;
        m_height = m_frame->height;
        
        if (!SDL_LockTexture(m_texture, NULL, &dst_pixels, &dst_pitch)) {
            SDL_UnlockMutex(m_mtxFrame);
            return;
        }
    }

    int src_linesize = m_frame->linesize[0];
    int width = m_frame->width;
    int height = m_frame->height;

    for (int y = 0; y < height; y++) {
        uint8_t *src_row = src_data + y * src_linesize;
        uint8_t *dst_row = (uint8_t *)dst_pixels + y * dst_pitch;
        memcpy(dst_row, src_row, width * 3);
    }
    SDL_UnlockMutex(m_mtxFrame);
    
    // Unlock texture to upload to GPU
    SDL_UnlockTexture(m_texture);
    
    // Clear and present
    SDL_RenderClear(m_renderer);
    SDL_RenderTexture(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);

}