module llmswitch.pet_packages;

import std;
import llmswitch.shimeji;
import llmswitch.pet_profile;

namespace pet_packages {
namespace {

namespace fs = std::filesystem;

struct TempDirectory {
    fs::path path;
    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void Release() noexcept { path.clear(); }
};

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

void ValidateTree(const fs::path& source) {
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) throw std::runtime_error("模型包不能包含符号链接");
        if (!entry.is_regular_file()) continue;
        if (++files > 2048) throw std::runtime_error("模型包文件数量超过限制");
        std::error_code ec;
        const std::uintmax_t size = entry.file_size(ec);
        if (ec) throw std::runtime_error("无法读取模型包文件大小");
        bytes += size;
        if (bytes > 512 * 1024 * 1024) throw std::runtime_error("模型包总大小超过限制");
    }
}

void ValidatePackage(Kind kind, const fs::path& path) {
    if (kind == Kind::shimeji) {
        static_cast<void>(shimeji::LoadPack(path));
    } else {
        static_cast<void>(pet_profile::ResolveLive2DModel(
            path.parent_path(), path.filename().string()));
    }
}

} // namespace

std::string Import(Kind kind, const fs::path& source,
                   const fs::path& destinationRoot) {
    std::error_code ec;
    fs::create_directories(destinationRoot, ec);
    if (ec) throw std::runtime_error("无法创建模型数据目录");
    const fs::path canonicalSource = fs::canonical(source, ec);
    const fs::path canonicalRoot = fs::canonical(destinationRoot, ec);
    if (ec || !fs::is_directory(canonicalSource, ec)) {
        throw std::runtime_error("导入源不是有效目录");
    }
    if (IsWithin(canonicalSource, canonicalRoot)) {
        throw std::runtime_error("不能把模型导入到其自身目录内部");
    }
    const std::string id = canonicalSource.filename().string();
    if (id.empty() || id == "." || id == "..") throw std::runtime_error("模型目录名称无效");
    const fs::path destination = canonicalRoot / id;
    if (fs::exists(destination, ec)) throw std::runtime_error("同名模型已存在，未覆盖");
    ValidateTree(canonicalSource);
    ValidatePackage(kind, canonicalSource);

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    TempDirectory temp{canonicalRoot / std::format(".importing-{}-{}", id, stamp)};
    fs::create_directories(temp.path, ec);
    if (ec) throw std::runtime_error("无法创建模型导入临时目录");
    fs::copy(canonicalSource, temp.path,
             fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    if (ec) throw std::runtime_error("复制模型包失败：" + ec.message());
    ValidatePackage(kind, temp.path);
    fs::rename(temp.path, destination, ec);
    if (ec) throw std::runtime_error("完成模型导入失败：" + ec.message());
    temp.Release();
    return id;
}

void Delete(const fs::path& destinationRoot, std::string_view id) {
    if (id.empty() || id == "." || id == ".." || id.find('/') != std::string_view::npos ||
        id.find('\\') != std::string_view::npos) {
        throw std::runtime_error("模型 id 无效");
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(destinationRoot, ec);
    const fs::path target = fs::weakly_canonical(destinationRoot / fs::path(id), ec);
    if (ec || target == root || !IsWithin(root, target) || !fs::is_directory(target, ec)) {
        throw std::runtime_error("只能删除应用数据目录内的模型");
    }
    fs::remove_all(target, ec);
    if (ec) throw std::runtime_error("删除模型失败：" + ec.message());
}

} // namespace pet_packages
