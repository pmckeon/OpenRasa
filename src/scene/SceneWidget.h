// SceneWidget.h - a UI SceneView widget's 3D view (the front end's FrontEnd/BkgdView: the login / character screens' backgrounds),
// drawn with RasaEdit's renderer and SceneView: its bodies (AddBody: a .geo each) at the origin, the light set the scripts give
// (SetupSceneView, gameuiutil.FrontEndLightInfo), the camera at SetCameraPos looking +z with the widget's fieldOfView, nearPlane and
// farPlane (the export's attributes; gfxView's projection FUN_005e7360: the field of view spans the view's LONGER side), cleared to
// SetClearColor.
#pragma once

#include "Renderer.h"
#include "SceneSettings.h"
#include "SceneView.h"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace scene {

// A body of the scripts (EntityBody, as snapshotted) in the scene's terms.
SceneView::ScriptBody scriptBodyOf(const PreviewBody& p);

class SceneWidget {
public:
    SceneWidget(SDL_GPUDevice* device, const tr::AssetLibrary& assets);
    ~SceneWidget();
    // Draws the view at width x height (its own colour target, returned) for the settings, dt seconds after the last frame.
    // UpdateCameraToHead requests are carried out here; the camera and projection they set are written back into settings.
    SDL_GPUTexture* render(SDL_GPUCommandBuffer* commands, SceneSettings& settings, int width, int height, float dt);

private:
    SDL_GPUDevice* device_;
    const tr::AssetLibrary& assets_;
    std::unique_ptr<Renderer> renderer_;
    uint32_t headDone_ = 0;               // the last UpdateCameraToHead request carried out
    std::unique_ptr<tr::MapBundle> map_;
    std::unique_ptr<SceneView> view_;
    std::vector<std::string> bodies_;        // the bodies view_ was built for
};

}  // namespace scene
