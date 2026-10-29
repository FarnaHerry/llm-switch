export module llmswitch.pet_packages;

import std;

namespace pet_packages {

export enum class Kind { shimeji, live2d };

// Validate and copy a user-selected local model directory into the application data root.
// Returns the relative model id. Existing destinations are never overwritten.
export std::string Import(Kind kind, const std::filesystem::path& source,
                          const std::filesystem::path& destinationRoot);
// Delete one direct child of the application data root after validating its boundary.
export void Delete(const std::filesystem::path& destinationRoot, std::string_view id);

} // namespace pet_packages
