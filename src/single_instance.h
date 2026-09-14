#pragma once

#include <functional>

namespace llmswitch::single_instance {

// Returns true when this process owns the instance. A second process forwards an activation and returns false.
[[nodiscard]] bool StartOrActivate();

// Installs the callback used to forward an activation onto the application's UI thread.
void SetActivationHandler(std::function<void()> handler);
void ClearActivationHandler() noexcept;
void Stop() noexcept;

} // namespace llmswitch::single_instance
