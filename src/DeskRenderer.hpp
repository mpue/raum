#pragma once

#include <memory>
#include <string>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Types.hpp>

namespace raum {

class Server;
class View;

// Draws the desk with fitzel: the hall (sky, floor with its grid, soft shadows
// and a faint reflection), every window as a textured quad, then hands the
// frame to the output. The scene goes into a render target of its own first --
// the output's buffer is upside down to OpenGL, and one flipped blit at the end
// is simpler than turning every pass round.
class DeskRenderer {
public:
    explicit DeskRenderer(Server& server);
    ~DeskRenderer();

    // The scene (no HUD yet) into the scene target, `width` x `height`.
    void render(int width, int height);
    // Bind the scene target for the HUD to draw over.
    void bindScene();
    // The finished frame to the window framebuffer (the output's buffer).
    void present();
    // The scene target's pixels as a PNG (top row first).
    bool savePng(const std::string& path);
    // What the output really got (after present): the buffer handed to the
    // display, read back. For checking the import path, not for everyday use.
    bool saveOutputPng(const std::string& path);

private:
    void updateCanvas(View* view);
    void drawSky();
    void drawFloor(bool reflectionPass);
    void drawWindows(bool reflection);
    void drawFrame(View* view, float alpha);

    Server& m_server;
    std::unique_ptr<fitzel::RenderTarget> m_scene;
    int m_w = 0, m_h = 0;
    fitzel::Shader m_skyShader, m_floorShader, m_windowShader, m_surfaceShader, m_glowShader;
    fitzel::Mesh m_quad;   // unit quad, corners at 0..1
};

} // namespace raum
