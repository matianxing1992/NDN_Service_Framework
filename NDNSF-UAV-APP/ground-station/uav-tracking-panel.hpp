#ifndef NDNSF_UAV_GROUND_STATION_TRACKING_PANEL_HPP
#define NDNSF_UAV_GROUND_STATION_TRACKING_PANEL_HPP

#include "uav-tracking-controls.hpp"

#include <gdkmm/pixbufloader.h>

namespace {

/** Decode with bounds installed before allocation, including hostile JPEG dimensions. */
Glib::RefPtr<Gdk::Pixbuf>
decodeTrackingJpeg(const TrackingFrameImage& image)
{
  if (!image.jpeg || image.jpeg->empty() || image.jpeg->size() > 4 * 1024 * 1024 ||
      image.width == 0 || image.width > MAX_TRACKING_DISPLAY_WIDTH || image.height == 0 || image.height > MAX_TRACKING_DISPLAY_HEIGHT) {
    throw std::invalid_argument("tracking JPEG input bounds");
  }
  auto loader = Gdk::PixbufLoader::create("jpeg");
  struct Bounds
  {
    const TrackingFrameImage* image;
    bool prepared = false;
    bool invalid = false;
  } bounds{&image};
  const auto prepare = +[] (GdkPixbufLoader* raw, int width, int height, void* opaque) noexcept {
    auto& state = *static_cast<Bounds*>(opaque);
    state.prepared = true;
    state.invalid = width <= 0 || height <= 0 ||
      static_cast<uint64_t>(width) > MAX_TRACKING_DISPLAY_WIDTH ||
      static_cast<uint64_t>(height) > MAX_TRACKING_DISPLAY_HEIGHT ||
      static_cast<uint64_t>(width) != state.image->width ||
      static_cast<uint64_t>(height) != state.image->height;
    if (state.invalid) gdk_pixbuf_loader_set_size(raw, 1, 1);
    else {
      const double scale = std::min(480.0 / width, 300.0 / height);
      gdk_pixbuf_loader_set_size(raw, std::max(1, static_cast<int>(width * scale)),
                                std::max(1, static_cast<int>(height * scale)));
    }
  };
  const auto handler = g_signal_connect_data(loader->gobj(), "size-prepared",
    reinterpret_cast<GCallback>(prepare), &bounds, nullptr, static_cast<GConnectFlags>(0));
  if (handler == 0) throw std::runtime_error("tracking JPEG bounds signal unavailable");
  // Own only the synchronous signal; disconnect before the stack data and loader are released.
  const auto disconnect = [handler] (GdkPixbufLoader* raw) {
    g_signal_handler_disconnect(raw, handler);
  };
  const std::unique_ptr<GdkPixbufLoader, decltype(disconnect)> connection(loader->gobj(), disconnect);
  loader->write(image.jpeg->data(), image.jpeg->size());
  loader->close();
  if (!bounds.prepared || bounds.invalid || !loader->get_pixbuf()) {
    throw std::invalid_argument("tracking JPEG actual dimensions mismatch");
  }
  return loader->get_pixbuf();
}

/** GTK owns the timer and widgets; Face callbacks only reach the controls' weak slot. */
class GroundStationTrackingPanel : public Gtk::Box
{
public:
  GroundStationTrackingPanel(GroundStationTrackingControls& controls,
                              std::function<bool()> stopRequested)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 12)
    , m_controls(controls)
    , m_stopRequested(std::move(stopRequested))
  {
    m_connections.reserve(6);
    set_border_width(16);
    m_heading.set_markup("<b>Multi-UAV tracking</b>");
    pack_start(m_heading, Gtk::PACK_SHRINK);
    m_description.set_text("Recorded camera video • simulated UAV topology • inference device shown below");
    pack_start(m_description, Gtk::PACK_SHRINK);
    m_actions.pack_start(m_configs, Gtk::PACK_EXPAND_WIDGET);
    m_actions.pack_start(m_load, Gtk::PACK_SHRINK);
    m_actions.pack_start(m_start, Gtk::PACK_SHRINK);
    m_actions.pack_start(m_cancel, Gtk::PACK_SHRINK);
    pack_start(m_actions, Gtk::PACK_SHRINK);
    pack_start(m_status, Gtk::PACK_SHRINK);
    m_status.set_line_wrap(true);
    m_status.set_xalign(0);
    pack_start(m_participants, Gtk::PACK_SHRINK);
    m_participants.set_line_wrap(true);
    m_participants.set_xalign(0);
    for (size_t i = 0; i < 3; ++i) {
      m_cameraBoxes[i].set_orientation(Gtk::ORIENTATION_VERTICAL);
      m_cameraBoxes[i].pack_start(m_drawings[i], Gtk::PACK_EXPAND_WIDGET);
      m_cameraBoxes[i].pack_start(m_captions[i], Gtk::PACK_SHRINK);
      m_drawings[i].set_size_request(320, 300);
      m_captions[i].set_line_wrap(true);
      m_frames[i].add(m_cameraBoxes[i]);
      m_cameras.pack_start(m_frames[i], Gtk::PACK_EXPAND_WIDGET);
      m_drawContexts[i] = {this, i};
      bindSignal(G_OBJECT(m_drawings[i].gobj()), "draw", reinterpret_cast<GCallback>(
        +[] (GtkWidget*, cairo_t* raw, void* data) noexcept -> gboolean {
          auto& drawing = *static_cast<DrawContext*>(data);
          return drawing.panel->guard([&] {
            const Cairo::RefPtr<Cairo::Context> context(new Cairo::Context(raw, false));
            return drawing.panel->draw(drawing.index, context);
          });
        }), &m_drawContexts[i]);
    }
    pack_start(m_cameras, Gtk::PACK_EXPAND_WIDGET);
    bindSignal(G_OBJECT(m_start.gobj()), "clicked", reinterpret_cast<GCallback>(
      +[] (GtkButton*, void* data) noexcept {
        auto& panel = *static_cast<GroundStationTrackingPanel*>(data);
        panel.guard([&] {
          const auto index = panel.m_configs.get_active_row_number();
          if (index >= 0) panel.m_controls.start(static_cast<size_t>(index));
          return true;
        });
      }), this);
    bindSignal(G_OBJECT(m_cancel.gobj()), "clicked", reinterpret_cast<GCallback>(
      +[] (GtkButton*, void* data) noexcept {
        auto& panel = *static_cast<GroundStationTrackingPanel*>(data);
        panel.guard([&] { panel.m_controls.cancel(); return true; });
      }), this);
    bindSignal(G_OBJECT(m_load.gobj()), "clicked", reinterpret_cast<GCallback>(
      +[] (GtkButton*, void* data) noexcept {
        auto& panel = *static_cast<GroundStationTrackingPanel*>(data);
        panel.guard([&] { panel.loadConfiguration(); return true; });
      }), this);
    m_timer.reset(g_timeout_source_new(200));
    g_source_set_callback(m_timer.get(), +[] (void* data) noexcept -> gboolean {
      auto& panel = *static_cast<GroundStationTrackingPanel*>(data);
      return panel.guard([&] { return panel.tick(); });
    }, this, nullptr);
    if (g_source_attach(m_timer.get(), nullptr) == 0) {
      throw std::runtime_error("tracking GTK timer could not be attached");
    }
  }

  ~GroundStationTrackingPanel() override
  {
    m_timer.reset();
    m_connections.clear();
  }

private:
#ifdef NDNSF_UAV_MAIN_APP_TEST_SEAM
  friend struct GroundStationTrackingPanelTestAccess;
#endif

  struct DrawContext
  {
    GroundStationTrackingPanel* panel = nullptr;
    size_t index = 0;
  };

  struct DisconnectSignal
  {
    gulong handler = 0;
    void operator()(GObject* object) const noexcept
    {
      g_signal_handler_disconnect(object, handler);
    }
  };

  static void
  releaseSource(GSource* source) noexcept
  {
    g_source_destroy(source);
    g_source_unref(source);
  }

  void
  bindSignal(GObject* object, const char* name, GCallback callback, void* data)
  {
    const auto handler = g_signal_connect_data(object, name, callback, data, nullptr,
                                               static_cast<GConnectFlags>(0));
    if (handler == 0) throw std::runtime_error("tracking GTK signal could not be connected");
    m_connections.emplace_back(object, DisconnectSignal{handler});
  }

  template<typename Action>
  bool
  guard(Action&& action) noexcept
  {
    try { return action(); }
    catch (const Glib::Error& error) { gtk_label_set_text(m_status.gobj(), error.what().c_str()); }
    catch (const std::exception& error) { gtk_label_set_text(m_status.gobj(), error.what()); }
    catch (...) { gtk_label_set_text(m_status.gobj(), "Tracking UI callback failed"); }
    return false;
  }

  void
  loadConfiguration()
  {
    Gtk::FileChooserDialog dialog("Load prepared tracking job", Gtk::FILE_CHOOSER_ACTION_OPEN);
    if (auto* window = dynamic_cast<Gtk::Window*>(get_toplevel())) dialog.set_transient_for(*window);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Load", Gtk::RESPONSE_OK);
    if (dialog.run() == Gtk::RESPONSE_OK) m_controls.addDefinition(dialog.get_filename());
  }

  bool
  tick()
  {
    if (m_stopRequested && m_stopRequested()) {
      if (auto* window = dynamic_cast<Gtk::Window*>(get_toplevel())) window->hide();
      return false;
    }
    m_controls.tick();
    const auto& definitions = m_controls.definitions();
    while (m_catalogSize < definitions.size()) {
      const auto& definition = definitions[m_catalogSize++];
      m_configs.append((definition.mode == TrackingMode::PERSON ? "Person | " : "Vehicle | ") +
                        definition.jobId);
    }
    if (m_configs.get_active_row_number() < 0 && m_catalogSize) m_configs.set_active(0);
    const auto& view = m_controls.view();
    for (size_t i = 0; i < definitions.size(); ++i) {
      if (definitions[i].runId == view.runId && m_controls.automatic()) m_configs.set_active(i);
    }
    const auto selected = m_configs.get_active_row_number();
    m_start.set_sensitive(selected >= 0 && m_controls.canStart(static_cast<size_t>(selected)));
    m_cancel.set_sensitive(m_controls.canCancel());
    m_load.set_sensitive(!m_controls.pending() && !m_controls.automatic() &&
      (view.state == TrackingJobState::IDLE || view.cleanupState == "CLEAN"));
    m_configs.set_sensitive(!m_controls.pending() && !m_controls.automatic());
    m_status.set_text(trackingStateText(view.state) + " | windows " +
      std::to_string(view.completedWindows) + "/" + std::to_string(view.totalWindows) +
      " | device " + view.actualDevice + " | cancel " + view.cancelState +
      " | cleanup " + view.cleanupState + "\n" + view.reason + " " + m_controls.message());
    const TrackingJobDefinition* definition = nullptr;
    for (const auto& candidate : definitions) if (candidate.runId == view.runId) definition = &candidate;
    if (definition) {
      m_description.set_text(definition->mode == TrackingMode::PERSON ?
        "Indoor person ReID • dataset video replay • simulated UAV topology • geometry UNKNOWN" :
        "Vehicle tracking • video replay • simulated geometry • illustrative/unvalidated measurements");
      std::string text = "Run: " + view.runId + " | compute: " + definition->computeIdentity.toUri();
      for (const auto& source : definition->sources) text += " | " + source.cameraId + ": " + source.droneId;
      m_participants.set_text(text);
    }
    const auto key = std::to_string(view.generation) + ":" + view.lastResultDigest;
    if (key != m_displayKey) {
      m_displayError.clear();
      for (auto& image : m_images) image.reset();
      m_bundle.reset();
      for (auto& drawing : m_drawings) drawing.queue_draw();
      // Wait until all original frames for this exact result have passed native admission.
      if (definition && view.presentation && view.presentation->resultDigest == view.lastResultDigest &&
          view.presentation->generation == view.generation) {
        try {
          auto projected = projectTrackingPresentation(*view.presentation, *definition,
                                                       view.generation, view.lastResultDigest);
          std::array<Glib::RefPtr<Gdk::Pixbuf>, 3> images;
          for (size_t i = 0; i < 3; ++i) images[i] = decodeTrackingJpeg(view.presentation->frames[i]);
          m_projected = std::move(projected);
          m_images = std::move(images);
          m_bundle = view.presentation;
          m_displayKey = key;
          m_drawn = {};
          for (size_t i = 0; i < 3; ++i) {
            const auto& frame = m_bundle->frames[i];
            m_frames[i].set_label(frame.cameraId);
            m_captions[i].set_text(frame.sourceId + " | epoch " + std::to_string(frame.sourceEpoch) +
              " | sequence " + std::to_string(frame.sequence));
            m_drawings[i].queue_draw();
          }
        }
        catch (const std::exception& error) { m_displayError = error.what(); m_displayKey = key; }
        catch (const Glib::Error& error) { m_displayError = error.what(); m_displayKey = key; }
      }
    }
    if (!m_displayError.empty()) m_status.set_text("Display rejected: " + m_displayError);
    return true;
  }

  bool
  draw(size_t index, const Cairo::RefPtr<Cairo::Context>& context)
  {
    const auto allocation = m_drawings[index].get_allocation();
    context->set_source_rgb(0.06, 0.07, 0.09);
    context->paint();
    const auto& image = m_images[index];
    if (!image || !m_bundle) return true;
    const double scale = std::min(static_cast<double>(allocation.get_width()) / image->get_width(),
                                  static_cast<double>(allocation.get_height()) / image->get_height());
    const double width = image->get_width() * scale, height = image->get_height() * scale;
    context->save();
    context->translate((allocation.get_width() - width) / 2, (allocation.get_height() - height) / 2);
    context->save();
    context->scale(scale, scale);
    Gdk::Cairo::set_source_pixbuf(context, image, 0, 0);
    context->paint();
    context->restore();
    context->set_line_width(2);
    context->set_font_size(12);
    for (const auto& box : m_projected[index].boxes) {
      const auto color = box.globalId % 5;
      static constexpr double COLORS[5][3] = {{0.1,1,0.3},{1,0.8,0.1},{0.2,0.8,1},{1,0.3,0.6},{0.8,0.6,1}};
      context->set_source_rgb(COLORS[color][0], COLORS[color][1], COLORS[color][2]);
      context->rectangle(box.box[0] * width, box.box[1] * height,
                         (box.box[2] - box.box[0]) * width, (box.box[3] - box.box[1]) * height);
      context->stroke();
      context->move_to(box.box[0] * width, std::max(14.0, box.box[1] * height - 4));
      context->show_text(box.label);
      if (box.arrow) {
        const auto& arrow = *box.arrow;
        context->move_to(arrow[0] * width, arrow[1] * height);
        context->line_to((arrow[0] + arrow[2]) * width, (arrow[1] + arrow[3]) * height);
        context->stroke();
      }
    }
    context->restore();
    if (!m_drawn[index]) {
      m_drawn[index] = true;
      std::cout << "SPEC228_TRACKING_FRAME_SHOWN run=" << m_bundle->runId
                << " window=" << m_bundle->windowIndex << " camera=" << m_projected[index].cameraId
                << " result=" << m_bundle->resultDigest << std::endl;
    }
    return true;
  }

  GroundStationTrackingControls& m_controls;
  std::function<bool()> m_stopRequested;
  Gtk::Label m_heading, m_description, m_status, m_participants;
  Gtk::Box m_actions{Gtk::ORIENTATION_HORIZONTAL, 8};
  Gtk::ComboBoxText m_configs;
  Gtk::Button m_load{"Load configuration"}, m_start{"Start"}, m_cancel{"Cancel"};
  Gtk::Box m_cameras{Gtk::ORIENTATION_HORIZONTAL, 12};
  std::array<Gtk::Frame, 3> m_frames;
  std::array<Gtk::Box, 3> m_cameraBoxes;
  std::array<Gtk::DrawingArea, 3> m_drawings;
  std::array<Gtk::Label, 3> m_captions;
  std::array<Glib::RefPtr<Gdk::Pixbuf>, 3> m_images;
  std::array<TrackingCameraDisplay, 3> m_projected;
  std::array<bool, 3> m_drawn{};
  std::shared_ptr<const TrackingPresentation> m_bundle;
  size_t m_catalogSize = 0;
  std::string m_displayKey;
  std::string m_displayError;
  std::array<DrawContext, 3> m_drawContexts;
  std::unique_ptr<GSource, void(*)(GSource*)> m_timer{nullptr, releaseSource};
  std::vector<std::unique_ptr<GObject, DisconnectSignal>> m_connections;
};

} // namespace

#endif // NDNSF_UAV_GROUND_STATION_TRACKING_PANEL_HPP
