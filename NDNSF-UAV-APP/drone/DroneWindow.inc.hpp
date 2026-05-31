// App-internal implementation chunk included by UavDroneApp.cpp.
// Contains only the GTK drone status window.

class DroneWindow : public Gtk::Window
{
public:
  explicit DroneWindow(DroneServiceContainer& runtime, std::string flightControllerStatusFile)
    : m_runtime(runtime)
    , m_flightControllerStatusFile(std::move(flightControllerStatusFile))
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
  {
    set_title("NDNSF UAV Drone");
    set_default_size(420, 180);
    set_border_width(12);

    m_title.set_markup("<b>Drone " + m_runtime.identityUri() + "</b>");
    m_status.set_text("Video stopped");
    m_flightControllerStatus.set_text(initialFlightControllerStatus());
    m_frames.set_text("Stream packets: 0, FEC groups: 0");

    m_box.pack_start(m_title, Gtk::PACK_SHRINK);
    m_box.pack_start(m_status, Gtk::PACK_SHRINK);
    m_box.pack_start(m_flightControllerStatus, Gtk::PACK_SHRINK);
    m_box.pack_start(m_frames, Gtk::PACK_SHRINK);
    add(m_box);
    show_all_children();

    m_runtime.setStatusCallback([this](std::string status) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pendingStatus = std::move(status);
      }
      m_dispatcher.emit();
    });
    m_dispatcher.connect([this] {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_status.set_text(m_pendingStatus);
    });
    Glib::signal_timeout().connect([this] {
      m_frames.set_text("Stream packets: " + std::to_string(m_runtime.streamPacketsPublished()) +
                        ", FEC groups: " + std::to_string(m_runtime.fecGroupsPublished()));
      m_flightControllerStatus.set_text(readFlightControllerStatus());
      if (!m_runtime.isStreaming()) {
        m_status.set_text("Video stopped");
      }
      return true;
    }, 500);
  }

private:
  std::string
  initialFlightControllerStatus() const
  {
    if (m_flightControllerStatusFile.empty()) {
      return "Flight controller: mock backend ready";
    }
    return "Flight controller: starting";
  }

  std::string
  readFlightControllerStatus() const
  {
    if (m_flightControllerStatusFile.empty()) {
      return "Flight controller: mock backend ready";
    }
    std::ifstream input(m_flightControllerStatusFile);
    std::string status;
    if (!std::getline(input, status) || status.empty()) {
      status = "starting";
    }
    return "Flight controller: " + status;
  }

  DroneServiceContainer& m_runtime;
  std::string m_flightControllerStatusFile;
  Gtk::Box m_box;
  Gtk::Label m_title;
  Gtk::Label m_status;
  Gtk::Label m_flightControllerStatus;
  Gtk::Label m_frames;
  Glib::Dispatcher m_dispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
};
