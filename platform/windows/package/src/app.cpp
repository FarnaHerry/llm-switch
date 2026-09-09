#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <installer_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;
namespace installer_strings = installer::strings;

namespace {

bool SystemPrefersDark() {
  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }

  DWORD apps_use_light_theme = 1;
  DWORD size = sizeof(apps_use_light_theme);
  const LONG result = RegQueryValueExA(key, "AppsUseLightTheme", nullptr, nullptr,
                                       reinterpret_cast<LPBYTE>(&apps_use_light_theme), &size);
  RegCloseKey(key);
  return result == ERROR_SUCCESS && apps_use_light_theme == 0;
}

ThemeSpec InkDarkThemeSpec() {
  ThemeSpec spec = MaterialDarkThemeSpec();
  spec.typography = TypographyScheme{
      .body_large = 16.0F,
      .body_medium = 14.0F,
      .body_small = 12.0F,
      .label_large = 14.0F,
      .title_large = 20.0F,
      .headline_small = 24.0F,
  };
  spec.colors.primary = Color::Rgb(230, 224, 210);          // 宣纸白
  spec.colors.on_primary = Color::Rgb(38, 35, 30);           // 浓墨
  spec.colors.secondary = Color::Rgb(179, 172, 156);         // 淡墨
  spec.colors.on_secondary = Color::Rgb(38, 35, 30);
  spec.colors.secondary_container = Color::Rgb(58, 54, 45);
  spec.colors.on_secondary_container = Color::Rgb(230, 224, 210);
  spec.colors.background = Color::Rgb(22, 20, 17);            // 玄墨海面
  spec.colors.surface = Color::Rgb(28, 26, 22);
  spec.colors.surface_container_low = Color::Rgb(33, 30, 26);
  spec.colors.surface_container = Color::Rgb(40, 37, 31);
  spec.colors.surface_container_high = Color::Rgb(47, 44, 37);
  spec.colors.surface_container_highest = Color::Rgb(56, 52, 44);
  spec.colors.on_surface = Color::Rgb(214, 208, 192);        // 宣纸灰
  spec.colors.on_surface_variant = Color::Rgb(163, 156, 139); // 淡墨
  spec.colors.outline = Color::Rgb(76, 71, 60);
  spec.colors.inverse_surface = Color::Rgb(214, 208, 192);
  spec.colors.inverse_on_surface = Color::Rgb(38, 35, 30);
  spec.colors.error = Color::Rgb(223, 114, 86);               // 朱砂
  return spec;
}

ThemeSpec InkLightThemeSpec() {
  ThemeSpec spec = MaterialLightThemeSpec();
  spec.typography = TypographyScheme{
      .body_large = 16.0F,
      .body_medium = 14.0F,
      .body_small = 12.0F,
      .label_large = 14.0F,
      .title_large = 20.0F,
      .headline_small = 24.0F,
  };
  spec.colors.primary = Color::Rgb(43, 40, 35);              // 浓墨
  spec.colors.on_primary = Color::Rgb(246, 243, 234);        // 宣纸白
  spec.colors.secondary = Color::Rgb(110, 105, 92);          // 淡墨
  spec.colors.on_secondary = Color::Rgb(248, 245, 238);
  spec.colors.secondary_container = Color::Rgb(227, 221, 203);
  spec.colors.on_secondary_container = Color::Rgb(43, 40, 35);
  spec.colors.background = Color::Rgb(239, 234, 224);         // 宣纸海面
  spec.colors.surface = Color::Rgb(247, 244, 236);
  spec.colors.surface_container_low = Color::Rgb(242, 238, 228);
  spec.colors.surface_container = Color::Rgb(248, 245, 236);
  spec.colors.surface_container_high = Color::Rgb(230, 225, 211);
  spec.colors.surface_container_highest = Color::Rgb(252, 250, 243);
  spec.colors.on_surface = Color::Rgb(46, 43, 37);           // 浓墨正文
  spec.colors.on_surface_variant = Color::Rgb(110, 105, 92); // 淡墨
  spec.colors.outline = Color::Rgb(216, 210, 194);
  spec.colors.inverse_surface = Color::Rgb(46, 43, 37);
  spec.colors.inverse_on_surface = Color::Rgb(246, 243, 234);
  spec.colors.error = Color::Rgb(181, 70, 46);                // 朱砂
  return spec;
}

ThemeSpec InstallerThemeSpec() {
  return SystemPrefersDark() ? InkDarkThemeSpec() : InkLightThemeSpec();
}

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

View InstallerTheme(View content) {
  const ThemeSpec spec = InstallerThemeSpec();
  ThemeDefinition definition = MaterialThemeDefinition(spec);

  ButtonStyle buttons = ButtonStyle::Default();
  buttons.background = spec.colors.primary;
  buttons.label_style = TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_primary};
  buttons.corner_radii = CornerRadii{8.0F};
  definition.Set(buttons);

  DialogStyle dialogs = DialogStyle::Default();
  dialogs.background = spec.colors.surface_container_high;
  dialogs.title_style = TextStyle{Font::System(spec.typography.title_large).WithWeight(FontWeight::Bold),
                                   spec.colors.on_surface};
  dialogs.message_style = TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_surface};
  dialogs.positive_action_background = spec.colors.primary;
  dialogs.positive_action_style = TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_primary};
  dialogs.negative_action_style = TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_surface};
  dialogs.action_separator_color = spec.colors.outline;
  definition.Set(dialogs);

  return Theme(std::move(definition), std::move(content));
}

} // namespace

std::string InstallPathText(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {value.begin(), value.end()};
}

std::optional<std::filesystem::path> ParseInstallPath(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  try {
    std::filesystem::path path(std::u8string(value.begin(), value.end()));
    return path.is_absolute() ? std::optional<std::filesystem::path>{path.lexically_normal()} : std::nullopt;
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
}

namespace {

std::filesystem::path UserDataDirectory() {
  const wchar_t* app_data = _wgetenv(L"APPDATA");
  if (app_data == nullptr || *app_data == L'\0') {
    return {};
  }
  return std::filesystem::path(app_data) / L"llm-switch";
}

void RemoveUserData() {
  const std::filesystem::path data_directory = UserDataDirectory();
  if (data_directory.empty()) {
    return;
  }

  // Keep this guard close to the destructive operation so a malformed or
  // missing APPDATA value can never turn into a broad recursive deletion.
  if (data_directory.filename() != std::filesystem::path(L"llm-switch")) {
    throw std::runtime_error("The llm-switch data directory could not be validated.");
  }

  std::error_code error;
  std::filesystem::remove_all(data_directory, error);
  if (error) {
    throw std::runtime_error("Could not remove llm-switch data: " + error.message());
  }
}

} // namespace

View InstallerMark(Color foreground, Color detail) {
  return Canvas([foreground, detail](PaintContext& paint, Size size) {
    const float extent = std::min(size.width, size.height);
    Color tile = foreground;
    tile.alpha = 0.10F;
    paint.DrawCircle({extent * 0.5F, extent * 0.5F}, extent * 0.44F, tile);
    paint.DrawArc({extent * 0.5F, extent * 0.5F}, extent * 0.34F, 0.0F, 3.14159F,
                  foreground, StrokeStyle{.width = 3.0F, .cap = StrokeCap::Round});
    paint.DrawArc({extent * 0.5F, extent * 0.5F}, extent * 0.34F, 3.14159F, 3.14159F,
                  detail, StrokeStyle{.width = 3.0F, .cap = StrokeCap::Round});
    paint.DrawCircle({extent * 0.5F, extent * 0.5F - extent * 0.18F}, extent * 0.045F, detail);
    paint.DrawCircle({extent * 0.5F, extent * 0.5F + extent * 0.18F}, extent * 0.045F, foreground);
  }).With(Frame{.width = 64.0F, .height = 64.0F});
}

View BrandPanel(const ThemeSpec& theme) {
  const Color panel_start = theme.colors.surface_container;
  const Color panel_end = theme.colors.background;
  const Color secondary_text = WithAlpha(theme.colors.on_surface_variant, 0.92F);

  return Column {
    InstallerMark(theme.colors.primary, theme.colors.error),
    Column {
      Text(installer_strings::application_setup)
          .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold), secondary_text}),
      Text("llm-switch")
          .Style(TextStyle{Font::System(28.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface}),
      Text(installer_strings::guided_setup)
          .Style(TextStyle{Font::System(theme.typography.body_medium), secondary_text}),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Start)),
    Spacer().With(Grow()),
    Text(installer_strings::platform_windows)
        .Style(TextStyle{Font::System(11.0F).WithWeight(FontWeight::SemiBold), secondary_text}),
  }.With(
      Frame{.width = 236.0F},
      Padding(28.0F),
      Spacing(26.0F),
      Background(LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {1.0F, 1.0F},
          .stops = {{0.0F, panel_start}, {1.0F, panel_end}},
      }),
      Border{WithAlpha(theme.colors.outline, 0.45F)},
      CrossAlign(CrossAxisAlignment::Start)
  );
}

View CompletionMark(const ThemeSpec& theme) {
  Color background = theme.colors.primary;
  background.alpha = 0.12F;
  return Canvas([background, foreground = theme.colors.primary](PaintContext& paint, Size) {
    paint.DrawCircle({26.0F, 26.0F}, 26.0F, background);
    const StrokeStyle stroke{.width = 3.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
    paint.DrawLine({15.0F, 27.0F}, {23.0F, 35.0F}, foreground, stroke);
    paint.DrawLine({23.0F, 35.0F}, {39.0F, 18.0F}, foreground, stroke);
  }).With(Frame{.width = 52.0F, .height = 52.0F});
}

View SecondaryAction(View action, const ThemeSpec& theme) {
  Color disabled_background = theme.colors.on_surface;
  disabled_background.alpha = 0.08F;
  Color disabled_label = theme.colors.on_surface;
  disabled_label.alpha = 0.38F;
  ThemeDefinition definition;
  definition.Set(ButtonStyle{
      .background = theme.colors.surface_container_highest,
      .label_style =
          TextStyle{Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium), theme.colors.on_surface},
      .disabled_background = disabled_background,
      .disabled_label = disabled_label,
      .padding = EdgeInsets::Symmetric(20.0F, 8.0F),
      .minimum_width = 72.0F,
      .minimum_height = 40.0F,
      .corner_radii = CornerRadii{theme.shapes.full},
      .indication = theme.interactions.indication,
  });
  return Theme {std::move(definition), std::move(action)};
}

std::vector<View> PromptActions(const InstallerHandle& installer, const InstallerPrompt& prompt,
                                const ThemeSpec& theme) {
  std::vector<View> actions;
  for (const InstallerPromptChoice choice : prompt.choices) {
    StringVariant label = installer_strings::continue_action;
    switch (choice) {
    case InstallerPromptChoice::Ok:
      label = installer_strings::ok;
      break;
    case InstallerPromptChoice::Cancel:
      label = installer_strings::cancel;
      break;
    case InstallerPromptChoice::Abort:
      label = installer_strings::abort;
      break;
    case InstallerPromptChoice::Retry:
      label = installer_strings::retry;
      break;
    case InstallerPromptChoice::TryAgain:
      label = installer_strings::try_again;
      break;
    case InstallerPromptChoice::Ignore:
      label = installer_strings::ignore;
      break;
    case InstallerPromptChoice::Yes:
      label = installer_strings::yes;
      break;
    case InstallerPromptChoice::No:
      label = installer_strings::no;
      break;
    case InstallerPromptChoice::Continue:
      label = installer_strings::continue_action;
      break;
    }
    View action = Button(std::move(label)).OnClick([installer, id = prompt.id, choice] {
      installer.Respond(id, choice);
    });
    if (!prompt.recommended || choice != *prompt.recommended) {
      action = SecondaryAction(std::move(action), theme);
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

[[huxerui::composable]]
View InstallerContent() {
  const InstallerHandle installer = UseInstaller();
  const WindowHandle window = UseWindow();
  const TaskScope tasks = UseTaskScope();
  const DialogHandle dialog = UseDialog();
  const ThemeSpec& theme = UseTheme();
  const InstallerStatus status = installer.Status();
  auto destination = UseState<std::optional<TextEditingValue>>(std::nullopt);
  auto desktop_shortcut = UseState<std::optional<bool>>(std::nullopt);
  StringVariant eyebrow;
  StringVariant heading;
  std::vector<View> details;
  std::vector<View> actions;
  MainAxisAlignment panel_alignment = MainAxisAlignment::Start;

  if (status.prompt) {
    eyebrow = installer_strings::action_required;
    heading = installer_strings::setup_needs_attention;
    StringVariant prompt_message = status.prompt->message;
    if (status.prompt->kind == InstallerPromptKind::FilesInUse && status.prompt->message.empty()) {
      prompt_message = installer_strings::files_in_use;
    }
    details.push_back(Text(std::move(prompt_message)).With(Foreground(theme.colors.on_surface_variant)));
    actions = PromptActions(installer, *status.prompt, theme);
  } else if (status.phase == InstallerPhase::Detecting) {
    eyebrow = installer_strings::welcome;
    heading = installer_strings::preparing_setup;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::checking_installed_version).With(Foreground(theme.colors.on_surface_variant))
    );
    details.push_back(ProgressBar(status.progress));
  } else if (status.phase == InstallerPhase::Ready &&
             status.product == InstallerProductState::NewerVersion) {
    eyebrow = installer_strings::version_check;
    heading = installer_strings::newer_version_installed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::remove_newer_version)
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::Present) {
    eyebrow = installer_strings::upgrade;
    heading = installer_strings::existing_installation_detected;
    details.push_back(
        Text(installer_strings::already_installed).With(Foreground(theme.colors.on_surface_variant))
    );
    details.push_back(
        Text(installer_strings::upgrade_keep_data_message).With(Foreground(theme.colors.on_surface_variant))
    );
    details.push_back(
        Text(installer_strings::uninstall_options).With(Foreground(theme.colors.on_surface_variant))
    );

    const auto show_uninstall_options = [dialog, tasks, installer] {
      dialog.Show(
          installer_strings::uninstall_title,
          installer_strings::uninstall_message,
          installer_strings::delete_data_and_uninstall,
          installer_strings::keep_data_and_uninstall,
          [dialog, tasks, installer] {
            tasks.Launch([dialog, installer]() -> Task<void> {
              try {
                co_await RunWorker([] { RemoveUserData(); });
                installer.Uninstall();
              } catch (const std::exception& error) {
                dialog.Show(
                    installer_strings::data_delete_failed,
                    error.what(),
                    installer_strings::close,
                    {},
                    {},
                    {},
                    {}
                );
              }
            });
          },
          [installer] { installer.Uninstall(); }
      );
    };

    actions.push_back(SecondaryAction(
        Button(installer_strings::uninstall).OnClick(show_uninstall_options), theme
    ));
    actions.push_back(
        Button(installer_strings::repair)
            .OnClick([installer] { installer.Repair(); })
            .With(Frame{.min_width = 96.0F})
    );
    actions.push_back(
        Button(installer_strings::upgrade_keep_data)
            .OnClick([installer] { installer.Install(); })
            .With(Frame{.min_width = 152.0F})
    );
  } else if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::Absent) {
    const TextEditingValue destination_value = destination.Get().value_or(
        TextEditingValue::FromText(InstallPathText(status.default_destination))
    );
    const std::optional<std::filesystem::path> destination_path = ParseInstallPath(destination_value.text);
    const std::filesystem::path browse_initial = destination_path.value_or(status.default_destination);
    const bool create_desktop_shortcut =
        desktop_shortcut.Get().value_or(status.default_create_desktop_shortcut);
    eyebrow = installer_strings::installation;
    heading = installer_strings::ready_to_install;
    details.push_back(
        Text(installer_strings::installation_options).With(Foreground(theme.colors.on_surface_variant))
    );
    details.push_back(Row {
      TextField(destination_value)
          .Label(installer_strings::installation_folder)
          .Variant(TextFieldVariant::Outlined)
          .Validation(
              destination_path ? ValidationResult::None()
                               : ValidationResult::Invalid(installer_strings::invalid_installation_folder)
          )
          .OnChanged([destination](const TextEditingValue& value) { destination = value; })
          .With(Grow()),
      SecondaryAction(
          Button(installer_strings::browse).OnClick([installer, destination, browse_initial, tasks] {
            tasks.Launch([installer, destination, browse_initial]() -> Task<void> {
              const std::optional<std::filesystem::path> selected =
                  co_await installer.ChooseDestinationAsync(browse_initial);
              if (selected) {
                destination = TextEditingValue::FromText(InstallPathText(*selected));
              }
            });
          }),
          theme
      ),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)));
    Color option_border = theme.colors.outline;
    option_border.alpha = 0.18F;
    details.push_back(
        Checkbox(installer_strings::create_desktop_shortcut, create_desktop_shortcut)
            .OnChanged([desktop_shortcut](bool enabled) { desktop_shortcut = enabled; })
            .With(
                Padding(EdgeInsets::Symmetric(14.0F, 10.0F)),
                Background(theme.colors.surface_container_low),
                Border{.color = option_border},
                CornerRadius(theme.shapes.medium)
            )
    );
    actions.push_back(
        SecondaryAction(Button(installer_strings::cancel).OnClick([window] { window.Close(); }), theme)
    );
    actions.push_back(
        Button(installer_strings::install)
            .OnClick([installer, destination_path, create_desktop_shortcut] {
              if (destination_path) {
                installer.Install({
                    .destination = *destination_path,
                    .create_desktop_shortcut = create_desktop_shortcut,
                });
              }
            })
            .With(Enabled(destination_path.has_value()), Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Ready) {
    eyebrow = installer_strings::setup;
    heading = installer_strings::setup_could_not_continue;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::installed_version_unknown).With(Foreground(theme.colors.on_surface_variant))
    );
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Planning || status.phase == InstallerPhase::Applying ||
             status.phase == InstallerPhase::Canceling) {
    eyebrow = installer_strings::installing;
    heading = installer_strings::installing_product;
    panel_alignment = MainAxisAlignment::Center;
    if (status.phase == InstallerPhase::Canceling) {
      eyebrow = installer_strings::rollback;
      heading = installer_strings::canceling_and_rolling_back;
    } else if (status.action == InstallerAction::Repair) {
      eyebrow = installer_strings::repairing;
      heading = installer_strings::repairing_product;
    } else if (status.action == InstallerAction::Uninstall) {
      eyebrow = installer_strings::removing;
      heading = installer_strings::uninstalling_product;
    }
    StringVariant progress_message = status.current_package.empty()
                                         ? StringVariant(installer_strings::operation_may_take_a_moment)
                                         : StringVariant(status.current_package);
    details.push_back(Text(std::move(progress_message)).With(Foreground(theme.colors.on_surface_variant)));
    details.push_back(ProgressBar(status.progress));
    actions.push_back(
        SecondaryAction(Button(installer_strings::cancel).OnClick([installer] { installer.Cancel(); }), theme)
    );
  } else if (status.phase == InstallerPhase::Completed) {
    eyebrow = installer_strings::complete;
    heading = status.action == InstallerAction::Uninstall ? installer_strings::application_removed
                                                          : installer_strings::installation_complete;
    panel_alignment = MainAxisAlignment::Center;
    StringVariant message;
    if (status.action == InstallerAction::Uninstall) {
      message = status.restart == InstallerRestart::Required ? installer_strings::application_removed_restart_message
                                                              : installer_strings::application_removed_message;
    } else {
      message = status.restart == InstallerRestart::Required
                    ? installer_strings::installation_complete_restart_message
                    : installer_strings::installation_complete_message;
    }
    details.push_back(Row {CompletionMark(theme)}.With(MainAlign(MainAxisAlignment::Center)));
    details.push_back(Text(std::move(message)).With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Canceled) {
    eyebrow = installer_strings::canceled;
    heading = installer_strings::installation_canceled;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::no_further_changes).With(Foreground(theme.colors.on_surface_variant))
    );
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.failure) {
    eyebrow = installer_strings::setup_error;
    heading = installer_strings::installation_failed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(status.failure->message).With(Foreground(theme.colors.error)));
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  }

  std::vector<View> panel{
      Text(std::move(eyebrow))
          .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold), theme.colors.primary}),
      Text(std::move(heading), TextRole::Title).With(FontSize(theme.typography.headline_small)),
  };
  panel.insert(panel.end(), std::make_move_iterator(details.begin()), std::make_move_iterator(details.end()));

  std::vector<View> content;
  content.push_back(Column(std::move(panel)).With(
      Spacing(16.0F),
      MainAlign(panel_alignment),
      CrossAlign(CrossAxisAlignment::Stretch),
      Grow()
  ));
  if (!actions.empty()) {
    content.push_back(Divider());
    content.push_back(Row(std::move(actions)).With(
        Spacing(10.0F),
        MainAlign(MainAxisAlignment::End),
        CrossAlign(CrossAxisAlignment::Center)
    ));
  }

  return Row {
    BrandPanel(theme),
    Column(std::move(content)).With(
        Padding(EdgeInsets::Symmetric(36.0F, 32.0F)),
        Spacing(24.0F),
        CrossAlign(CrossAxisAlignment::Stretch),
        Grow()
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.surface), Grow());
}

View InstallerPage() {
  return InstallerTheme(InstallerContent());
}

const Application application{
    InstallerPage,
    {
        .window = {
            .title = "llm-switch",
            .initial_size = {780.0F, 500.0F},
            .minimum_size = Size{720.0F, 460.0F},
        },
        .show_debug_overlay = false,
        .root_hooks = {InstallInstallerSession},
    },
};
