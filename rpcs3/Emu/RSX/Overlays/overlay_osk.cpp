#include "stdafx.h"
#include "overlay_osk.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellKb.h"

LOG_CHANNEL(osk, "OSK");

namespace rsx
{
	namespace overlays
	{
		osk_dialog::osk_dialog()
		{
			m_auto_repeat_buttons.insert(pad_button::L1);
			m_auto_repeat_buttons.insert(pad_button::R1);
			m_auto_repeat_buttons.insert(pad_button::cross);
			m_auto_repeat_buttons.insert(pad_button::triangle);
			m_auto_repeat_buttons.insert(pad_button::square);

			m_keyboard_input_enabled = true;
		}

		void osk_dialog::Close(s32 status)
		{
			fade_animation.current = color4f(1.f);
			fade_animation.end = color4f(0.f);
			fade_animation.duration = 0.5f;

			fade_animation.on_finish = [this, status]
			{
				if (on_osk_close)
				{
					Emu.CallFromMainThread([this, status]()
					{
						on_osk_close(status);
					});
				}

				visible = false;
				close(true, true);
			};

			fade_animation.active = true;
		}

		void osk_dialog::add_panel(const osk_panel& panel)
		{
			// On PS3 apparently only 7 panels are added, the rest is ignored
			if (m_panels.size() < 7)
			{
				// Don't add this panel if there already exists one with the same panel mode
				if (std::none_of(m_panels.begin(), m_panels.end(), [&panel](const osk_panel& existing) { return existing.osk_panel_mode == panel.osk_panel_mode; }))
				{
					m_panels.push_back(panel);
				}
			}
		}

		void osk_dialog::step_panel(bool next_panel)
		{
			const usz num_panels = m_panels.size();

			if (num_panels > 0)
			{
				if (next_panel)
				{
					m_panel_index = (m_panel_index + 1) % num_panels;
				}
				else if (m_panel_index > 0)
				{
					m_panel_index = (m_panel_index - 1) % num_panels;
				}
				else
				{
					m_panel_index = num_panels - 1;
				}
			}

			update_panel();
		}

		void osk_dialog::update_panel()
		{
			ensure(m_panel_index < m_panels.size());

			const auto& panel = m_panels[m_panel_index];

			num_rows = panel.num_rows;
			num_columns = panel.num_columns;
			cell_size_x = get_scaled(panel.cell_size_x);
			cell_size_y = get_scaled(panel.cell_size_y);

			update_layout();

			const u32 cell_count = num_rows * num_columns;

			m_grid.resize(cell_count);
			num_shift_layers_by_charset.clear();

			const position2u grid_origin(m_frame.x, m_frame.y + m_title.h + m_preview.h);

			const u32 old_index = (selected_y * num_columns) + selected_x;

			u32 index = 0;

			for (const auto& props : panel.layout)
			{
				for (u32 c = 0; c < props.num_cell_hz; ++c)
				{
					const auto row = (index / num_columns);
					const auto col = (index % num_columns);
					ensure(row < num_rows && col < num_columns);

					auto& _cell = m_grid[index++];
					_cell.button_flag = props.type_flags;
					_cell.pos = { grid_origin.x + col * cell_size_x, grid_origin.y + row * cell_size_y };
					_cell.backcolor = props.color;
					_cell.callback = props.callback;
					_cell.outputs = props.outputs;
					_cell.selected = false;

					// Add shift layers
					for (u32 layer = 0; layer < _cell.outputs.size(); ++layer)
					{
						// Only add a shift layer if at least one default button has content in a layer

						if (props.type_flags != button_flags::_default)
						{
							continue;
						}

						usz cell_shift_layers = 0;

						for (usz i = 0; i < _cell.outputs[layer].size(); ++i)
						{
							if (_cell.outputs[layer][i].empty() == false)
							{
								cell_shift_layers = i + 1;
							}
						}

						if (layer >= num_shift_layers_by_charset.size())
						{
							num_shift_layers_by_charset.push_back(static_cast<u32>(cell_shift_layers));
						}
						else
						{
							num_shift_layers_by_charset[layer] = std::max(num_shift_layers_by_charset[layer], static_cast<u32>(cell_shift_layers));
						}
					}

					switch (props.type_flags)
					{
					default:
					case button_flags::_default:
						_cell.enabled = true;
						break;
					case button_flags::_space:
						_cell.enabled = !(flags & CELL_OSKDIALOG_NO_SPACE);
						break;
					case button_flags::_return:
						_cell.enabled = !(flags & CELL_OSKDIALOG_NO_RETURN);
						break;
					case button_flags::_shift:
						_cell.enabled |= !_cell.outputs.empty();
						break;
					case button_flags::_layer:
						_cell.enabled |= !num_shift_layers_by_charset.empty();
						break;
					}

					if (props.num_cell_hz == 1) [[likely]]
					{
						_cell.flags = border_flags::default_cell;
					}
					else if (c == 0)
					{
						// Leading cell
						_cell.flags = border_flags::start_cell;
					}
					else if (c == (props.num_cell_hz - 1))
					{
						// Last cell
						_cell.flags = border_flags::end_cell;
					}
					else
					{
						// Middle cell
						_cell.flags = border_flags::middle_cell;
					}
				}
			}

			ensure(num_shift_layers_by_charset.size());

			for (u32 layer = 0; layer < num_shift_layers_by_charset.size(); ++layer)
			{
				ensure(num_shift_layers_by_charset[layer]);
			}

			// Reset to first shift layer in the first charset, because the panel changed and we don't know if the layers are similar between panels.
			m_selected_charset = 0;
			selected_z = 0;

			// Enable/Disable the control buttons based on the current layout.
			update_controls();

			// Roughly keep x and y selection in grid if possible. Jumping to (0,0) would be annoying. Needs to be done after updating the control buttons.
			update_selection_by_index(old_index);

			m_update = true;
		}

		void osk_dialog::update_layout()
		{
			const u16 title_height = get_scaled(30);
			const u16 preview_height = get_scaled((flags & CELL_OSKDIALOG_NO_RETURN) ? 40 : 90);

			// Place elements with absolute positioning
			const u16 button_margin = get_scaled(30);
			const u16 button_height = get_scaled(30);
			const u16 frame_w = num_columns * cell_size_x;
			const u16 frame_h = num_rows * cell_size_y + title_height + preview_height;
			f32 origin_x = 0.0f;
			f32 origin_y = 0.0f;

			switch (m_x_align)
			{
			case CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_RIGHT:
				origin_x = virtual_width;
				break;
			case CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER:
				origin_x = static_cast<f32>(virtual_width - frame_w) / 2.0f;
				break;
			case CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT:
			default:
				break;
			}

			switch (m_y_align)
			{
			case CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_BOTTOM:
				origin_y = virtual_height;
				break;
			case CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER:
				origin_y = static_cast<f32>(virtual_height - frame_h) / 2.0f;
				break;
			case CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP:
			default:
				break;
			}

			// TODO: does the y offset need to be added or subtracted?

			// Calculate initial position and analog movement range.
			constexpr f32 margin = 50.0f; // Let's add a minimal margin on all sides
			const u16 x_min = static_cast<u16>(margin);
			const u16 x_max = static_cast<u16>(static_cast<f32>(virtual_width - frame_w) - margin);
			const u16 y_min = static_cast<u16>(margin);
			const u16 y_max = static_cast<u16>(static_cast<f32>(virtual_height - (frame_h + button_height + button_margin)) - margin);
			u16 frame_x = 0;
			u16 frame_y = 0;

			// x pos should only be 0 the first time
			if (m_x_pos == 0)
			{
				frame_x = m_x_pos = static_cast<u16>(std::clamp<f32>(origin_x + m_x_offset, x_min, x_max));
				frame_y = m_y_pos = static_cast<u16>(std::clamp<f32>(origin_y + m_y_offset, y_min, y_max));
			}
			else
			{
				frame_x = m_x_pos = std::clamp(m_x_pos, x_min, x_max);
				frame_y = m_y_pos = std::clamp(m_y_pos, y_min, y_max);
			}

			m_frame.set_pos(frame_x, frame_y);
			m_frame.set_size(frame_w, frame_h);

			m_title.set_pos(frame_x, frame_y);
			m_title.set_size(frame_w, title_height);
			m_title.set_padding(get_scaled(15), 0, get_scaled(5), 0);

			m_preview.set_pos(frame_x, frame_y + title_height);
			m_preview.set_size(frame_w, preview_height);
			m_preview.set_padding(get_scaled(15), 0, get_scaled(10), 0);

			m_btn_cancel.set_pos(frame_x, frame_y + frame_h + button_margin);
			m_btn_cancel.set_size(get_scaled(140), button_height);
			m_btn_cancel.set_text(localized_string_id::RSX_OVERLAYS_OSK_DIALOG_CANCEL);
			m_btn_cancel.set_text_vertical_adjust(get_scaled(5));

			m_btn_space.set_pos(frame_x + get_scaled(100), frame_y + frame_h + button_margin);
			m_btn_space.set_size(get_scaled(100), button_height);
			m_btn_space.set_text(localized_string_id::RSX_OVERLAYS_OSK_DIALOG_SPACE);
			m_btn_space.set_text_vertical_adjust(get_scaled(5));

			m_btn_delete.set_pos(frame_x + get_scaled(200), frame_y + frame_h + button_margin);
			m_btn_delete.set_size(get_scaled(100), button_height);
			m_btn_delete.set_text(localized_string_id::RSX_OVERLAYS_OSK_DIALOG_BACKSPACE);
			m_btn_delete.set_text_vertical_adjust(get_scaled(5));

			m_btn_shift.set_pos(frame_x + get_scaled(320), frame_y + frame_h + button_margin);
			m_btn_shift.set_size(get_scaled(80), button_height);
			m_btn_shift.set_text(localized_string_id::RSX_OVERLAYS_OSK_DIALOG_SHIFT);
			m_btn_shift.set_text_vertical_adjust(get_scaled(5));

			m_btn_accept.set_pos(frame_x + get_scaled(400), frame_y + frame_h + button_margin);
			m_btn_accept.set_size(get_scaled(100), button_height);
			m_btn_accept.set_text(localized_string_id::RSX_OVERLAYS_OSK_DIALOG_ACCEPT);
			m_btn_accept.set_text_vertical_adjust(get_scaled(5));

			m_update = true;
		}

		void osk_dialog::initialize_layout(const std::u32string& title, const std::u32string& initial_text)
		{
			const auto scale_font = [this](overlay_element& elem)
			{
				if (const font* fnt = elem.get_font())
				{
					elem.set_font(fnt->get_name().data(), get_scaled(fnt->get_size_pt()));
				}
			};

			m_pointer.set_color(color4f{ 1.f, 1.f, 1.f, 1.f });

			m_background.set_size(virtual_width, virtual_height);

			m_title.set_unicode_text(title);
			m_title.back_color.a = 0.7f; // Uses the dimmed color of the frame background
			scale_font(m_title);

			m_preview.password_mode = m_password_mode;
			m_preview.set_placeholder(get_placeholder());
			m_preview.set_unicode_text(initial_text);
			scale_font(m_preview);

			if (m_preview.value.empty())
			{
				m_preview.caret_position = 0;
				m_preview.fore_color.a = 0.5f; // Muted contrast for hint text
			}
			else
			{
				m_preview.caret_position = m_preview.value.length();
				m_preview.fore_color.a = 1.f;
			}

			scale_font(m_btn_shift);
			scale_font(m_btn_accept);
			scale_font(m_btn_space);
			scale_font(m_btn_delete);
			scale_font(m_btn_cancel);

			m_btn_shift.text_horizontal_offset = get_scaled(m_btn_shift.text_horizontal_offset);
			m_btn_accept.text_horizontal_offset = get_scaled(m_btn_accept.text_horizontal_offset);
			m_btn_space.text_horizontal_offset = get_scaled(m_btn_space.text_horizontal_offset);
			m_btn_delete.text_horizontal_offset = get_scaled(m_btn_delete.text_horizontal_offset);
			m_btn_cancel.text_horizontal_offset = get_scaled(m_btn_cancel.text_horizontal_offset);

			m_btn_shift.set_image_resource(resource_config::standard_image_resource::select);
			m_btn_accept.set_image_resource(resource_config::standard_image_resource::start);
			m_btn_space.set_image_resource(resource_config::standard_image_resource::triangle);
			m_btn_delete.set_image_resource(resource_config::standard_image_resource::square);

			if (g_cfg.sys.enter_button_assignment == enter_button_assign::circle)
			{
				m_btn_cancel.set_image_resource(resource_config::standard_image_resource::cross);
			}
			else
			{
				m_btn_cancel.set_image_resource(resource_config::standard_image_resource::circle);
			}

			m_update = true;
			visible = true;
			m_stop_input_loop = false;

			fade_animation.current = color4f(0.f);
			fade_animation.end = color4f(1.f);
			fade_animation.duration = 0.5f;
			fade_animation.active = true;
		}

		void osk_dialog::update_controls()
		{
			const bool shift_enabled = num_shift_layers_by_charset[m_selected_charset] > 1;
			const bool layer_enabled = num_shift_layers_by_charset.size() > 1;

			for (auto& cell : m_grid)
			{
				switch (cell.button_flag)
				{
				case button_flags::_shift:
					cell.enabled = shift_enabled;
					break;
				case button_flags::_layer:
					cell.enabled = layer_enabled;
					break;
				default:
					break;
				}
			}

			m_update = true;
		}

		std::pair<u32, u32> osk_dialog::get_cell_geometry(u32 index)
		{
			const u32 grid_size = num_columns * num_rows;
			u32 start_index = index;
			u32 count = 0;

			while (start_index >= grid_size && start_index >= num_columns)
			{
				// Try one row above
				start_index -= num_columns;
			}

			// Find first cell
			while (!(m_grid[start_index].flags & border_flags::left) && start_index)
			{
				--start_index;
			}

			// Find last cell
			while (true)
			{
				const u32 current_index = (start_index + count);
				ensure(current_index < grid_size);
				++count;

				if (m_grid[current_index].flags & border_flags::right)
				{
					break;
				}
			}

			return std::make_pair(start_index, count);
		}

		void osk_dialog::update_selection_by_index(u32 index)
		{
			auto select_cell = [&](u32 i, bool state)
			{
				const auto info = get_cell_geometry(i);

				// Tag all in range
				for (u32 _index = info.first, _ctr = 0; _ctr < info.second; ++_index, ++_ctr)
				{
					m_grid[_index].selected = state;
				}
			};

			// 1. Deselect current
			const auto current_index = (selected_y * num_columns) + selected_x;
			select_cell(current_index, false);

			// 2. Select new
			selected_y = index / num_columns;
			selected_x = index % num_columns;
			select_cell(index, true);
		}

		void osk_dialog::on_button_pressed(pad_button button_press)
		{
			if (!pad_input_enabled || ignore_input_events)
				return;

			const u32 grid_size = num_columns * num_rows;

			const auto on_accept = [this]()
			{
				const u32 current_index = (selected_y * num_columns) + selected_x;
				const auto& current_cell = m_grid[current_index];

				u32 output_count = 0;

				if (m_selected_charset < current_cell.outputs.size())
				{
					output_count = ::size32(current_cell.outputs[m_selected_charset]);
				}

				if (output_count)
				{
					const auto _z = std::clamp<u32>(selected_z, 0u, output_count - 1u);
					const auto& str = current_cell.outputs[m_selected_charset][_z];

					if (current_cell.callback)
					{
						current_cell.callback(str);
					}
					else
					{
						on_default_callback(str);
					}
				}
			};

			// Increase auto repeat interval for some buttons
			switch (button_press)
			{
			case pad_button::rs_left:
			case pad_button::rs_right:
			case pad_button::rs_down:
			case pad_button::rs_up:
				m_auto_repeat_ms_interval = 10;
				break;
			default:
				m_auto_repeat_ms_interval = m_auto_repeat_ms_interval_default;
				break;
			}

			bool play_cursor_sound = true;

			switch (button_press)
			{
			case pad_button::L1:
			{
				m_preview.move_caret(edit_text::direction::left);
				m_update = true;
				break;
			}
			case pad_button::R1:
			{
				m_preview.move_caret(edit_text::direction::right);
				m_update = true;
				break;
			}
			case pad_button::dpad_right:
			case pad_button::ls_right:
			{
				u32 current_index = (selected_y * num_columns) + selected_x;
				while (true)
				{
					const auto current = get_cell_geometry(current_index);
					current_index = current.first + current.second;

					if (current_index >= grid_size)
					{
						break;
					}

					if (m_grid[get_cell_geometry(current_index).first].enabled)
					{
						update_selection_by_index(current_index);
						break;
					}
				}
				m_reset_pulse = true;
				break;
			}
			case pad_button::dpad_left:
			case pad_button::ls_left:
			{
				u32 current_index = (selected_y * num_columns) + selected_x;
				while (current_index > 0)
				{
					const auto current = get_cell_geometry(current_index);
					if (current.first)
					{
						current_index = current.first - 1;

						if (m_grid[get_cell_geometry(current_index).first].enabled)
						{
							update_selection_by_index(current_index);
							break;
						}
					}
					else
					{
						break;
					}
				}
				m_reset_pulse = true;
				break;
			}
			case pad_button::dpad_down:
			case pad_button::ls_down:
			{
				u32 current_index = (selected_y * num_columns) + selected_x;
				while (true)
				{
					current_index += num_columns;
					if (current_index >= grid_size)
					{
						break;
					}

					if (m_grid[get_cell_geometry(current_index).first].enabled)
					{
						update_selection_by_index(current_index);
						break;
					}
				}
				m_reset_pulse = true;
				break;
			}
			case pad_button::dpad_up:
			case pad_button::ls_up:
			{
				u32 current_index = (selected_y * num_columns) + selected_x;
				while (current_index >= num_columns)
				{
					current_index -= num_columns;
					if (m_grid[get_cell_geometry(current_index).first].enabled)
					{
						update_selection_by_index(current_index);
						break;
					}
				}
				m_reset_pulse = true;
				break;
			}
			case pad_button::select:
			{
				on_shift(U"");
				break;
			}
			case pad_button::start:
			{
				Emu.GetCallbacks().play_sound(fs::get_config_dir() + "sounds/snd_oskenter.wav");
				Close(CELL_OSKDIALOG_CLOSE_CONFIRM);
				play_cursor_sound = false;
				break;
			}
			case pad_button::triangle:
			{
				on_space(U"");
				break;
			}
			case pad_button::square:
			{
				on_backspace(U"");
				break;
			}
			case pad_button::cross:
			{
				Emu.GetCallbacks().play_sound(fs::get_config_dir() + "sounds/snd_oskenter.wav");
				on_accept();
				m_reset_pulse = true;
				play_cursor_sound = false;
				break;
			}
			case pad_button::circle:
			{
				Emu.GetCallbacks().play_sound(fs::get_config_dir() + "sounds/snd_oskcancel.wav");
				Close(CELL_OSKDIALOG_CLOSE_CANCEL);
				play_cursor_sound = false;
				break;
			}
			case pad_button::L2:
			{
				step_panel(false);
				break;
			}
			case pad_button::R2:
			{
				step_panel(true);
				break;
			}
			case pad_button::rs_left:
			case pad_button::rs_right:
			case pad_button::rs_down:
			case pad_button::rs_up:
			{
				if (!(flags & CELL_OSKDIALOG_NO_INPUT_ANALOG))
				{
					switch (button_press)
					{
					case pad_button::rs_left:  m_x_pos -= 5; break;
					case pad_button::rs_right: m_x_pos += 5; break;
					case pad_button::rs_down:  m_y_pos += 5; break;
					case pad_button::rs_up:    m_y_pos -= 5; break;
					default: break;
					}
					update_panel();
				}
				play_cursor_sound = false;
				break;
			}
			default:
				break;
			}

			if (play_cursor_sound)
			{
				Emu.GetCallbacks().play_sound(fs::get_config_dir() + "sounds/snd_cursor.wav");
			}

			if (m_reset_pulse)
			{
				m_update = true;
			}
		}

		void osk_dialog::on_key_pressed(u32 led, u32 mkey, u32 key_code, u32 out_key_code, bool pressed, std::u32string key)
		{
			if (!pressed || !keyboard_input_enabled || ignore_input_events)
				return;

			const bool use_key_string_fallback = !key.empty();

			osk.error("osk_dialog::on_key_pressed(led=%d, mkey=%d, key_code=%d, out_key_code=%d, pressed=%d, use_key_string_fallback=%d)", led, mkey, key_code, out_key_code, pressed, use_key_string_fallback);

			if (!use_key_string_fallback)
			{
				// Get keyboard layout
				const u32 kb_mapping = static_cast<u32>(g_cfg.sys.keyboard_type.get());

				// Convert key to its u32string presentation
				const u16 converted_out_key = cellKbCnvRawCode(kb_mapping, mkey, led, out_key_code);
				std::u16string utf16_string;
				utf16_string.push_back(converted_out_key);
				key = utf16_to_u32string(utf16_string);
			}

			// Find matching key in the OSK
			const auto find_key = [&]() -> bool
			{
				for (const cell& current_cell : m_grid)
				{
					for (const auto& output : current_cell.outputs)
					{
						for (const auto& str : output)
						{
							if (str == key)
							{
								// Apply key press
								if (current_cell.callback)
								{
									current_cell.callback(str);
								}
								else
								{
									on_default_callback(str);
								}

								return true;
							}
						}
					}
				}

				return false;
			};

			const bool found_key = find_key();

			if (use_key_string_fallback)
			{
				// We don't have a keycode, so there we can't process any of the following code anyway
				return;
			}

			// Handle special input
			if (!found_key)
			{
				switch (out_key_code)
				{
				case CELL_KEYC_SPACE:
					on_space(key);
					break;
				case CELL_KEYC_BS:
					on_backspace(key);
					break;
				case CELL_KEYC_DELETE:
					on_delete(key);
					break;
				case CELL_KEYC_ESCAPE:
					Close(CELL_OSKDIALOG_CLOSE_CANCEL);
					break;
				case CELL_KEYC_RIGHT_ARROW:
					on_move_cursor(key, edit_text::direction::right);
					break;
				case CELL_KEYC_LEFT_ARROW:
					on_move_cursor(key, edit_text::direction::left);
					break;
				case CELL_KEYC_DOWN_ARROW:
					on_move_cursor(key, edit_text::direction::down);
					break;
				case CELL_KEYC_UP_ARROW:
					on_move_cursor(key, edit_text::direction::up);
					break;
				case CELL_KEYC_ENTER:
					if ((flags & CELL_OSKDIALOG_NO_RETURN))
					{
						Close(CELL_OSKDIALOG_CLOSE_CONFIRM);
					}
					else
					{
						on_enter(key);
					}
					break;
				default:
					break;
				}
			}

			if (on_osk_key_input_entered)
			{
				CellOskDialogKeyMessage key_message{};
				key_message.led = led;
				key_message.mkey = mkey;
				key_message.keycode = key_code;
				on_osk_key_input_entered(key_message);
			}
		}

		void osk_dialog::on_text_changed()
		{
			const auto ws = u32string_to_utf16(m_preview.value);
			const auto length = (ws.length() + 1) * sizeof(char16_t);
			memcpy(osk_text, ws.c_str(), length);

			// Muted contrast for placeholder text
			m_preview.fore_color.a = m_preview.value.empty() ? 0.5f : 1.f;

			m_update = true;
		}

		void osk_dialog::on_default_callback(const std::u32string& str)
		{
			if (str.empty())
			{
				return;
			}

			// Append to output text
			if (m_preview.value.empty())
			{
				m_preview.caret_position = str.length();
				m_preview.set_unicode_text(str);
			}
			else
			{
				if (m_preview.value.length() == char_limit)
				{
					return;
				}

				const auto new_str = m_preview.value + str;
				if (new_str.length() <= char_limit)
				{
					m_preview.insert_text(str);
				}
			}

			on_text_changed();
		}

		void osk_dialog::on_shift(const std::u32string&)
		{
			const u32 max = num_shift_layers_by_charset[m_selected_charset];
			selected_z = (selected_z + 1) % max;
			m_update = true;
		}

		void osk_dialog::on_layer(const std::u32string&)
		{
			const u32 num_charsets = std::max<u32>(::size32(num_shift_layers_by_charset), 1);
			m_selected_charset = (m_selected_charset + 1) % num_charsets;

			const u32 max_z_layer = num_shift_layers_by_charset[m_selected_charset] - 1;

			if (selected_z > max_z_layer)
			{
				selected_z = max_z_layer;
			}

			update_controls();

			m_update = true;
		}

		void osk_dialog::on_space(const std::u32string&)
		{
			if (!(flags & CELL_OSKDIALOG_NO_SPACE))
			{
				on_default_callback(U" ");
			}
			else
			{
				// Beep or give some other kind of visual feedback
			}
		}

		void osk_dialog::on_backspace(const std::u32string&)
		{
			m_preview.erase();
			on_text_changed();
		}

		void osk_dialog::on_delete(const std::u32string&)
		{
			m_preview.del();
			on_text_changed();
		}

		void osk_dialog::on_enter(const std::u32string&)
		{
			if (!(flags & CELL_OSKDIALOG_NO_RETURN))
			{
				on_default_callback(U"\n");
			}
			else
			{
				// Beep or give some other kind of visual feedback
			}
		}

		void osk_dialog::on_move_cursor(const std::u32string&, edit_text::direction dir)
		{
			m_preview.move_caret(dir);
			m_update = true;
		}

		std::u32string osk_dialog::get_placeholder() const
		{
			const localized_string_id id = m_password_mode
				? localized_string_id::RSX_OVERLAYS_OSK_DIALOG_ENTER_PASSWORD
				: localized_string_id::RSX_OVERLAYS_OSK_DIALOG_ENTER_TEXT;
			return get_localized_u32string(id);
		}

		void osk_dialog::update()
		{
			if (fade_animation.active)
			{
				fade_animation.update(rsx::get_current_renderer()->vblank_count);
				m_update = true;
			}

			osk_info& info = g_fxo->get<osk_info>();

			if (const bool pointer_enabled = info.pointer_enabled; pointer_enabled != m_pointer.visible())
			{
				m_pointer.set_expiration(pointer_enabled ? u64{umax} : 0);
				m_pointer.update_visibility(get_system_time());
				m_update = true;
			}

			if (m_pointer.visible() && m_pointer.set_position(static_cast<u16>(info.pointer_x), static_cast<u16>(info.pointer_y)))
			{
				m_update = true;
			}
		}

		compiled_resource osk_dialog::get_compiled()
		{
			if (!visible)
			{
				return {};
			}

			if (m_update)
			{
				m_cached_resource.clear();
				m_cached_resource.add(m_background.get_compiled());
				m_cached_resource.add(m_frame.get_compiled());
				m_cached_resource.add(m_title.get_compiled());
				m_cached_resource.add(m_preview.get_compiled());
				m_cached_resource.add(m_btn_accept.get_compiled());
				m_cached_resource.add(m_btn_cancel.get_compiled());
				m_cached_resource.add(m_btn_shift.get_compiled());
				m_cached_resource.add(m_btn_space.get_compiled());
				m_cached_resource.add(m_btn_delete.get_compiled());

				overlay_element tmp;
				u16 buffered_cell_count = 0;
				bool render_label = false;

				const color4f disabled_back_color = { 0.3f, 0.3f, 0.3f, 1.f };
				const color4f disabled_fore_color = { 0.8f, 0.8f, 0.8f, 1.f };
				const color4f normal_fore_color = { 0.f, 0.f, 0.f, 1.f };

				label label;
				label.back_color = { 0.f, 0.f, 0.f, 0.f };
				label.set_padding(0, 0, get_scaled(10), 0);

				const auto scale_font = [this](overlay_element& elem)
				{
					if (const font* fnt = elem.get_font())
					{
						elem.set_font(fnt->get_name().data(), get_scaled(fnt->get_size_pt()));
					}
				};
				scale_font(label);

				if (m_reset_pulse)
				{
					// Reset the pulse slightly above 0 falling on each user interaction
					m_key_pulse_cache.set_sinus_offset(0.6f);
				}

				for (const auto& c : m_grid)
				{
					u16 x = static_cast<u16>(c.pos.x);
					u16 y = static_cast<u16>(c.pos.y);
					u16 w = cell_size_x;
					u16 h = cell_size_y;

					if (c.flags & border_flags::left)
					{
						x++;
						w--;
						buffered_cell_count = 0;
					}

					if (c.flags & border_flags::right)
					{
						w--;

						u32 output_count = 0;

						if (m_selected_charset < c.outputs.size())
						{
							output_count = ::size32(c.outputs[m_selected_charset]);
						}

						if (output_count)
						{
							const u16 offset_x = static_cast<u16>(buffered_cell_count * cell_size_x);
							const u16 full_width = static_cast<u16>(offset_x + cell_size_x);

							label.set_pos(x - offset_x, y);
							label.set_size(full_width, cell_size_y);
							label.fore_color = c.enabled ? normal_fore_color : disabled_fore_color;

							const auto _z = (selected_z < output_count) ? selected_z : output_count - 1u;
							label.set_unicode_text(c.outputs[m_selected_charset][_z]);
							label.align_text(rsx::overlays::overlay_element::text_align::center);
							render_label = true;
						}
					}

					if (c.flags & border_flags::top)
					{
						y++;
						h--;
					}

					if (c.flags & border_flags::bottom)
					{
						h--;
					}

					buffered_cell_count++;

					tmp.back_color = c.enabled? c.backcolor : disabled_back_color;
					tmp.set_pos(x, y);
					tmp.set_size(w, h);
					tmp.pulse_effect_enabled = c.selected;
					tmp.pulse_sinus_offset = m_key_pulse_cache.pulse_sinus_offset;

					m_cached_resource.add(tmp.get_compiled());

					if (render_label)
					{
						label.pulse_effect_enabled = c.selected;
						label.pulse_sinus_offset = m_key_pulse_cache.pulse_sinus_offset;
						m_cached_resource.add(label.get_compiled());
					}
				}

				m_cached_resource.add(m_pointer.get_compiled());
				m_reset_pulse = false;
				m_update = false;
			}

			fade_animation.apply(m_cached_resource);
			return m_cached_resource;
		}

		struct osk_dialog_thread
		{
			static constexpr auto thread_name = "OSK Thread"sv;
		};

		void osk_dialog::Create(const osk_params& params)
		{
			state = OskDialogState::Open;
			flags = params.prohibit_flags;
			char_limit = params.charlimit;
			m_x_align = params.x_align;
			m_y_align = params.y_align;
			m_x_offset = params.x_offset;
			m_y_offset = params.y_offset;
			m_scaling = params.initial_scale;
			m_frame.back_color.r = params.base_color.r;
			m_frame.back_color.g = params.base_color.g;
			m_frame.back_color.b = params.base_color.b;
			m_frame.back_color.a = params.base_color.a;
			m_background.back_color.a = params.dimmer_enabled ? 0.8f : 0.0f;
			m_start_pad_interception = params.intercept_input;

			const callback_t shift_cb  = [this](const std::u32string& text){ on_shift(text); };
			const callback_t layer_cb  = [this](const std::u32string& text){ on_layer(text); };
			const callback_t space_cb  = [this](const std::u32string& text){ on_space(text); };
			const callback_t delete_cb = [this](const std::u32string& text){ on_backspace(text); };
			const callback_t enter_cb  = [this](const std::u32string& text){ on_enter(text); };

			const auto is_supported = [&](u32 mode) -> bool
			{
				switch (mode)
				{
				case CELL_OSKDIALOG_PANELMODE_POLISH:
				case CELL_OSKDIALOG_PANELMODE_KOREAN:
				case CELL_OSKDIALOG_PANELMODE_TURKEY:
				case CELL_OSKDIALOG_PANELMODE_TRADITIONAL_CHINESE:
				case CELL_OSKDIALOG_PANELMODE_SIMPLIFIED_CHINESE:
				case CELL_OSKDIALOG_PANELMODE_PORTUGUESE_BRAZIL:
				case CELL_OSKDIALOG_PANELMODE_DANISH:
				case CELL_OSKDIALOG_PANELMODE_SWEDISH:
				case CELL_OSKDIALOG_PANELMODE_NORWEGIAN:
				case CELL_OSKDIALOG_PANELMODE_FINNISH:
					return (params.panel_flag & mode) && (params.support_language & mode);
				default:
					return (params.panel_flag & mode);
				}
			};

			const auto has_language_support = [&](CellSysutilLang language)
			{
				switch (language)
				{
				case CELL_SYSUTIL_LANG_KOREAN: return is_supported(CELL_OSKDIALOG_PANELMODE_KOREAN);
				case CELL_SYSUTIL_LANG_FINNISH: return is_supported(CELL_OSKDIALOG_PANELMODE_FINNISH);
				case CELL_SYSUTIL_LANG_SWEDISH: return is_supported(CELL_OSKDIALOG_PANELMODE_SWEDISH);
				case CELL_SYSUTIL_LANG_DANISH: return is_supported(CELL_OSKDIALOG_PANELMODE_DANISH);
				case CELL_SYSUTIL_LANG_NORWEGIAN: return is_supported(CELL_OSKDIALOG_PANELMODE_NORWEGIAN);
				case CELL_SYSUTIL_LANG_POLISH: return is_supported(CELL_OSKDIALOG_PANELMODE_POLISH);
				case CELL_SYSUTIL_LANG_PORTUGUESE_BR: return is_supported(CELL_OSKDIALOG_PANELMODE_PORTUGUESE_BRAZIL);
				case CELL_SYSUTIL_LANG_TURKISH: return is_supported(CELL_OSKDIALOG_PANELMODE_TURKEY);
				case CELL_SYSUTIL_LANG_CHINESE_T: return is_supported(CELL_OSKDIALOG_PANELMODE_TRADITIONAL_CHINESE);
				case CELL_SYSUTIL_LANG_CHINESE_S: return is_supported(CELL_OSKDIALOG_PANELMODE_SIMPLIFIED_CHINESE);
				default: return true;
				}
			};

			if (params.panel_flag & CELL_OSKDIALOG_PANELMODE_PASSWORD)
			{
				// If password was requested, then password has to be the only osk panel mode available to the user
				// first_view_panel can be ignored

				add_panel(osk_panel_password(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));

				m_password_mode = true;
			}
			else if (params.panel_flag == CELL_OSKDIALOG_PANELMODE_DEFAULT || params.panel_flag == CELL_OSKDIALOG_PANELMODE_DEFAULT_NO_JAPANESE)
			{
				// Prefer the systems settings
				// first_view_panel is ignored

				CellSysutilLang language = g_cfg.sys.language;

				// Fall back to english if the panel is not supported
				if (!has_language_support(language))
				{
					language = CELL_SYSUTIL_LANG_ENGLISH_US;
				}

				switch (g_cfg.sys.language)
				{
				case CELL_SYSUTIL_LANG_JAPANESE:
					if (params.panel_flag == CELL_OSKDIALOG_PANELMODE_DEFAULT_NO_JAPANESE)
						add_panel(osk_panel_english(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					else
						add_panel(osk_panel_japanese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_FRENCH:
					add_panel(osk_panel_french(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_SPANISH:
					add_panel(osk_panel_spanish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_GERMAN:
					add_panel(osk_panel_german(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_ITALIAN:
					add_panel(osk_panel_italian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_DANISH:
					add_panel(osk_panel_danish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_NORWEGIAN:
					add_panel(osk_panel_norwegian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_DUTCH:
					add_panel(osk_panel_dutch(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_FINNISH:
					add_panel(osk_panel_finnish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_SWEDISH:
					add_panel(osk_panel_swedish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_PORTUGUESE_PT:
					add_panel(osk_panel_portuguese_pt(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_PORTUGUESE_BR:
					add_panel(osk_panel_portuguese_br(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_TURKISH:
					add_panel(osk_panel_turkey(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_POLISH:
					add_panel(osk_panel_polish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_RUSSIAN:
					add_panel(osk_panel_russian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_KOREAN:
					add_panel(osk_panel_korean(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_CHINESE_T:
					add_panel(osk_panel_traditional_chinese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_CHINESE_S:
					add_panel(osk_panel_simplified_chinese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				case CELL_SYSUTIL_LANG_ENGLISH_US:
				case CELL_SYSUTIL_LANG_ENGLISH_GB:
				default:
					add_panel(osk_panel_english(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
					break;
				}
			}
			else
			{
				// Append osk modes.

				// TODO: find out the exact order

				if (is_supported(CELL_OSKDIALOG_PANELMODE_LATIN))
				{
					add_panel(osk_panel_latin(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_ENGLISH))
				{
					add_panel(osk_panel_english(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_FRENCH))
				{
					add_panel(osk_panel_french(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_SPANISH))
				{
					add_panel(osk_panel_spanish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_ITALIAN))
				{
					add_panel(osk_panel_italian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_GERMAN))
				{
					add_panel(osk_panel_german(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_TURKEY))
				{
					add_panel(osk_panel_turkey(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_POLISH))
				{
					add_panel(osk_panel_polish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_RUSSIAN))
				{
					add_panel(osk_panel_russian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_DANISH))
				{
					add_panel(osk_panel_danish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_NORWEGIAN))
				{
					add_panel(osk_panel_norwegian(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_DUTCH))
				{
					add_panel(osk_panel_dutch(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_SWEDISH))
				{
					add_panel(osk_panel_swedish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_FINNISH))
				{
					add_panel(osk_panel_finnish(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_PORTUGUESE))
				{
					add_panel(osk_panel_portuguese_pt(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_PORTUGUESE_BRAZIL))
				{
					add_panel(osk_panel_portuguese_br(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_KOREAN))
				{
					add_panel(osk_panel_korean(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_TRADITIONAL_CHINESE))
				{
					add_panel(osk_panel_traditional_chinese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_SIMPLIFIED_CHINESE))
				{
					add_panel(osk_panel_simplified_chinese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_JAPANESE))
				{
					add_panel(osk_panel_japanese(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_JAPANESE_HIRAGANA))
				{
					add_panel(osk_panel_japanese_hiragana(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_JAPANESE_KATAKANA))
				{
					add_panel(osk_panel_japanese_katakana(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_ALPHABET))
				{
					add_panel(osk_panel_alphabet_half_width(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_ALPHABET_FULL_WIDTH))
				{
					add_panel(osk_panel_alphabet_full_width(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_NUMERAL))
				{
					add_panel(osk_panel_numeral_half_width(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_NUMERAL_FULL_WIDTH))
				{
					add_panel(osk_panel_numeral_full_width(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}
				if (is_supported(CELL_OSKDIALOG_PANELMODE_URL))
				{
					add_panel(osk_panel_url(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
				}

				// Get initial panel based on first_view_panel
				for (usz i = 0; i < m_panels.size(); ++i)
				{
					if (params.first_view_panel == m_panels[i].osk_panel_mode)
					{
						m_panel_index = i;
						break;
					}
				}
			}

			// Fallback to english in case we forgot something
			if (m_panels.empty())
			{
				osk.error("No OSK panel found. Using english panel.");
				add_panel(osk_panel_english(shift_cb, layer_cb, space_cb, delete_cb, enter_cb));
			}

			initialize_layout(utf16_to_u32string(params.message), utf16_to_u32string(params.init_text));

			update_panel();

			auto& osk_thread = g_fxo->get<named_thread<osk_dialog_thread>>();

			const auto notify = std::make_shared<atomic_t<bool>>(false);

			osk_thread([&, notify]()
			{
				const u64 tbit = alloc_thread_bit();
				g_thread_bit = tbit;

				*notify = true;
				notify->notify_one();

				if (const auto error = run_input_loop())
				{
					if (error != selection_code::canceled)
					{
						rsx_log.error("Osk input loop exited with error code=%d", error);
					}
				}

				thread_bits &= ~tbit;
				thread_bits.notify_all();
			});

			while (osk_thread < thread_state::errored && !*notify)
			{
				notify->wait(false, atomic_wait_timeout{1'000'000});
			}
		}
	}
}
