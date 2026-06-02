// App-internal implementation chunk included by UavGroundStationApp.cpp.
// Contains only the GTK ground-station window and input/map presentation logic.

class GroundStationWindow : public Gtk::Window
{
public:
  GroundStationWindow(GroundStationServiceContainer& runtime, bool autoStart,
                      int autoStopSeconds, int autoStartDelayMs,
                      bool autoMavlinkTest, bool autoKeyboardTest,
                      bool autoManualControlTest,
                      bool autoTwoDroneSwitchTest,
                      bool autoRecordingPlaybackTest,
                      bool autoRepeatStopTest,
                      std::vector<std::string> droneIds)
    : m_runtime(runtime)
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
    , m_buttons(Gtk::ORIENTATION_HORIZONTAL, 8)
    , m_patrolControls(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_workspace(Gtk::ORIENTATION_HORIZONTAL, 10)
    , m_vehicleFrame("Vehicles")
    , m_vehiclePanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_centerFrame("Fly View")
    , m_centerPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_flyContent(Gtk::ORIENTATION_HORIZONTAL, 8)
    , m_mapFrame("Map / Mission")
    , m_mapPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_mapControls(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_videoFrame("Video Downlink")
    , m_videoPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_statusFrame("Inspector")
    , m_statusPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_start("Start Video")
    , m_stop("Stop Video")
    , m_arm("Arm")
    , m_takeoff("Takeoff")
    , m_land("Land")
    , m_emergencyStop("Emergency Stop")
    , m_patrol("Upload Patrol Mission")
    , m_startMission("Start Mission")
    , m_stopPatrol("Stop Patrol")
    , m_controlToggle("Start Control")
    , m_refreshRecording("Find Recordings")
    , m_playRecording("Play Recording")
    , m_mapZoomIn("+")
    , m_mapZoomOut("-")
    , m_mapCenterGs("Center GS")
    , m_mapUndoWp("Undo WP")
    , m_mapClearWp("Clear WPs")
    , m_controlPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_inputModeRow(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_keyboardMode("Keyboard")
    , m_gamepadMode("Xbox Gamepad")
    , m_controlLayout(Gtk::ORIENTATION_HORIZONTAL, 10)
    , m_keyboardLeftFrame("Keyboard Left Stick")
    , m_keyboardRightFrame("Keyboard Right Stick")
    , m_keyboardCommandFrame("Commands")
    , m_gamepadFrame("Xbox Gamepad")
    , m_keyW("W  Forward")
    , m_keyA("A  Yaw Left")
    , m_keyS("S  Back")
    , m_keyD("D  Yaw Right")
    , m_keyQ("Q  Roll Left")
    , m_keyE("E  Roll Right")
    , m_keyR("R  Throttle Up")
    , m_keyF("F  Throttle Down")
    , m_keyI("I  Arm")
    , m_keyT("T  Takeoff")
    , m_keyL("L  Land")
    , m_keyV("V  Video")
    , m_keyX("X  Stop Video")
    , m_padLeftStick("Left Stick\nYaw / Throttle")
    , m_padRightStick("Right Stick\nRoll / Pitch")
    , m_padA("A  Arm")
    , m_padB("B  Land")
    , m_padX("X  Video")
    , m_padY("Y  Takeoff")
    , m_padLB("LB")
    , m_padRB("RB")
    , m_autoRepeatStopTest(autoRepeatStopTest)
    , m_droneIds(std::move(droneIds))
  {
    set_title("NDNSF UAV Ground Station");
    set_default_size(1180, 740);
    set_border_width(12);
    set_can_focus(true);
    if (m_droneIds.empty()) {
      m_droneIds.push_back("A");
    }

    m_status.set_text("Video stopped");
    m_stats.set_text("Frames: 0");
    m_linkStatus.set_text("Link RTT: waiting");
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "GS center: University of Memphis\n"
                          "Selected drone: " + m_runtime.targetDroneId() + "\n"
                          "Markers: GS, drone A/B, and mission waypoints\n"
                          "Click to append WP1/WP2/..., drag to pan, Center GS to return.\n"
                          "Upload Patrol Mission sends the route; arm/takeoff/mission mode makes PX4 fly it.");
    m_services.set_text(m_runtime.serviceCatalogForDrone(m_runtime.targetDroneId()));
    m_telemetry.set_text("Telemetry: waiting for flight-controller response");
    m_stop.set_sensitive(false);
    m_startMission.set_sensitive(false);
    m_stopPatrol.set_sensitive(false);

    m_buttons.pack_start(m_start, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_stop, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_arm, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_takeoff, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_land, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_emergencyStop, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_patrol, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_startMission, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_stopPatrol, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_controlToggle, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_refreshRecording, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_playRecording, Gtk::PACK_SHRINK);
    m_box.pack_start(m_buttons, Gtk::PACK_SHRINK);

    m_patrolHint.set_text("Patrol center / size");
    m_patrolLat.set_text("35.1186");
    m_patrolLon.set_text("-89.9375");
    m_patrolSizeMeters.set_text("140");
    m_patrolLat.set_width_chars(9);
    m_patrolLon.set_width_chars(10);
    m_patrolSizeMeters.set_width_chars(6);
    m_patrolControls.pack_start(m_patrolHint, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLat, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLon, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolSizeMeters, Gtk::PACK_SHRINK);
    m_box.pack_start(m_patrolControls, Gtk::PACK_SHRINK);

    m_vehiclePanel.set_border_width(8);
    m_vehicleHint.set_text("Connected / expected drones");
    m_vehiclePanel.pack_start(m_vehicleHint, Gtk::PACK_SHRINK);
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      const auto selected = i == 0;
      auto* rowLabel = Gtk::manage(new Gtk::Label(
        std::string(selected ? "● " : "○ ") + "Drone " + m_droneIds[i] +
        (selected ? "  active" : "  standby")));
      rowLabel->set_xalign(0.0F);
      m_vehicleList.append(*rowLabel);
    }
    m_vehicleList.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
      if (row == nullptr) {
        return;
      }
      const auto index = row->get_index();
      if (index < 0 || static_cast<size_t>(index) >= m_droneIds.size()) {
        return;
      }
      m_runtime.setTargetDroneId(m_droneIds[static_cast<size_t>(index)]);
      m_runtime.logServiceCatalogForDrone(m_droneIds[static_cast<size_t>(index)]);
      updateVehicleRows();
      updateVideoViewForSelected();
      m_runtime.requestTelemetryStatus();
    });
    m_vehiclePanel.pack_start(m_vehicleList, Gtk::PACK_SHRINK);
    m_vehicleFrame.add(m_vehiclePanel);
    m_workspace.pack_start(m_vehicleFrame, Gtk::PACK_SHRINK);

    m_centerPanel.set_border_width(8);
    m_mapMission.set_xalign(0.0F);
    m_mapZoomIn.set_tooltip_text("Zoom in");
    m_mapZoomOut.set_tooltip_text("Zoom out");
    m_mapCenterGs.set_tooltip_text("Return map view to the ground station");
    m_mapUndoWp.set_tooltip_text("Remove the last mission waypoint");
    m_mapClearWp.set_tooltip_text("Clear all mission waypoints");
    m_mapControls.pack_start(m_mapZoomIn, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapZoomOut, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapCenterGs, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapUndoWp, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapClearWp, Gtk::PACK_SHRINK);
    m_mapPanel.set_border_width(6);
    m_mapPanel.pack_start(m_mapControls, Gtk::PACK_SHRINK);
    m_mapImage.set_size_request(256, 256);
    m_mapEventBox.set_size_request(256, 256);
    m_mapImage.set_halign(Gtk::ALIGN_START);
    m_mapImage.set_valign(Gtk::ALIGN_START);
    m_mapEventBox.set_halign(Gtk::ALIGN_START);
    m_mapEventBox.set_valign(Gtk::ALIGN_START);
    m_mapEventBox.add(m_mapImage);
    m_mapEventBox.add_events(Gdk::BUTTON_PRESS_MASK |
                             Gdk::BUTTON_RELEASE_MASK |
                             Gdk::POINTER_MOTION_MASK |
                             Gdk::SCROLL_MASK |
                             Gdk::SMOOTH_SCROLL_MASK);
    m_mapEventBox.signal_button_press_event().connect([this](GdkEventButton* event) {
      if (event == nullptr || event->button != 1) {
        return false;
      }
      beginMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_button_release_event().connect([this](GdkEventButton* event) {
      if (event == nullptr || event->button != 1) {
        return false;
      }
      finishMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_motion_notify_event().connect([this](GdkEventMotion* event) {
      if (event == nullptr || !m_mapDragging) {
        return false;
      }
      updateMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_scroll_event().connect([this](GdkEventScroll* event) {
      if (event == nullptr) {
        return false;
      }
      if (event->direction == GDK_SCROLL_UP ||
          (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0.0)) {
        zoomMap(1);
        return true;
      }
      if (event->direction == GDK_SCROLL_DOWN ||
          (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0.0)) {
        zoomMap(-1);
        return true;
      }
      return false;
    });
    m_mapPanel.pack_start(m_mapEventBox, Gtk::PACK_SHRINK);
    m_mapPanel.pack_start(m_mapMission, Gtk::PACK_SHRINK);
    m_mapFrame.add(m_mapPanel);
    m_flyContent.pack_start(m_mapFrame, Gtk::PACK_SHRINK);
    m_videoPanel.set_border_width(6);
    m_image.set_size_request(420, 260);
    m_image.set_halign(Gtk::ALIGN_CENTER);
    m_image.set_valign(Gtk::ALIGN_CENTER);
    m_videoPanel.pack_start(m_image, Gtk::PACK_EXPAND_WIDGET);
    m_videoPanel.pack_start(m_stats, Gtk::PACK_SHRINK);
    m_videoFrame.add(m_videoPanel);
    m_flyContent.pack_start(m_videoFrame, Gtk::PACK_EXPAND_WIDGET);
    m_centerPanel.pack_start(m_flyContent, Gtk::PACK_EXPAND_WIDGET);
    m_centerFrame.add(m_centerPanel);
    m_workspace.pack_start(m_centerFrame, Gtk::PACK_EXPAND_WIDGET);

    m_statusPanel.set_border_width(8);
    m_vehicleFrame.set_size_request(260, -1);
    m_vehicleFrame.set_hexpand(false);
    m_statusFrame.set_size_request(260, -1);
    m_statusFrame.set_hexpand(false);
    m_status.set_xalign(0.0F);
    m_stats.set_xalign(0.0F);
    m_services.set_xalign(0.0F);
    m_telemetry.set_xalign(0.0F);
    m_linkStatus.set_xalign(0.0F);
    for (auto* label : {&m_status, &m_linkStatus, &m_services, &m_telemetry}) {
      label->set_size_request(232, -1);
      label->set_width_chars(28);
      label->set_max_width_chars(28);
      label->set_line_wrap(true);
      label->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
      label->set_ellipsize(Pango::ELLIPSIZE_NONE);
    }
    m_statusPanel.pack_start(m_status, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_linkStatus, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_services, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_telemetry, Gtk::PACK_SHRINK);
    m_statusFrame.add(m_statusPanel);
    m_workspace.pack_start(m_statusFrame, Gtk::PACK_SHRINK);

    m_gamepadDevicePath = findJoystickDevice();
    m_gamepadAvailable = !m_gamepadDevicePath.empty();
    m_controlHelp.set_text(
      "Manual control uses QGroundControl-style sticks: left stick is yaw/throttle, "
      "right stick is roll/pitch. Active keys or gamepad controls turn black.");
    m_controlPanel.pack_start(m_controlHelp, Gtk::PACK_SHRINK);
    m_inputModeHint.set_text("Control input");
    m_inputModeHint.set_xalign(0.0F);
    m_keyboardMode.set_active(true);
    m_gamepadMode.set_sensitive(m_gamepadAvailable);
    m_gamepadMode.set_tooltip_text(m_gamepadAvailable ?
      "Use the first Linux joystick device: " + m_gamepadDevicePath :
      "No /dev/input/js* gamepad is readable");
    m_inputModeRow.pack_start(m_inputModeHint, Gtk::PACK_SHRINK);
    m_inputModeRow.pack_start(m_keyboardMode, Gtk::PACK_SHRINK);
    m_inputModeRow.pack_start(m_gamepadMode, Gtk::PACK_SHRINK);
    m_controlPanel.pack_start(m_inputModeRow, Gtk::PACK_SHRINK);

    m_keyboardLeftGrid.set_row_spacing(4);
    m_keyboardLeftGrid.set_column_spacing(4);
    m_keyboardRightGrid.set_row_spacing(4);
    m_keyboardRightGrid.set_column_spacing(4);
    m_keyboardCommandGrid.set_row_spacing(4);
    m_keyboardCommandGrid.set_column_spacing(4);
    m_gamepadGrid.set_row_spacing(4);
    m_gamepadGrid.set_column_spacing(4);
    m_keyboardLeftLabel.set_text("Yaw / Throttle");
    m_keyboardRightLabel.set_text("Roll / Pitch");
    m_keyboardLeftGrid.attach(m_keyR, 1, 0, 1, 1);
    m_keyboardLeftGrid.attach(m_keyA, 0, 1, 1, 1);
    m_keyboardLeftGrid.attach(m_keyboardLeftLabel, 1, 1, 1, 1);
    m_keyboardLeftGrid.attach(m_keyD, 2, 1, 1, 1);
    m_keyboardLeftGrid.attach(m_keyF, 1, 2, 1, 1);
    m_keyboardRightGrid.attach(m_keyW, 1, 0, 1, 1);
    m_keyboardRightGrid.attach(m_keyQ, 0, 1, 1, 1);
    m_keyboardRightGrid.attach(m_keyboardRightLabel, 1, 1, 1, 1);
    m_keyboardRightGrid.attach(m_keyE, 2, 1, 1, 1);
    m_keyboardRightGrid.attach(m_keyS, 1, 2, 1, 1);
    m_keyboardCommandGrid.attach(m_keyI, 0, 0, 1, 1);
    m_keyboardCommandGrid.attach(m_keyT, 1, 0, 1, 1);
    m_keyboardCommandGrid.attach(m_keyL, 2, 0, 1, 1);
    m_keyboardCommandGrid.attach(m_keyV, 0, 1, 1, 1);
    m_keyboardCommandGrid.attach(m_keyX, 1, 1, 1, 1);
    m_keyboardLeftFrame.add(m_keyboardLeftGrid);
    m_keyboardRightFrame.add(m_keyboardRightGrid);
    m_keyboardCommandFrame.add(m_keyboardCommandGrid);
    m_controlLayout.pack_start(m_keyboardLeftFrame, Gtk::PACK_SHRINK);
    m_controlLayout.pack_start(m_keyboardRightFrame, Gtk::PACK_SHRINK);
    m_controlLayout.pack_start(m_keyboardCommandFrame, Gtk::PACK_SHRINK);

    m_gamepadGrid.attach(m_padY, 1, 0, 1, 1);
    m_gamepadGrid.attach(m_padX, 0, 1, 1, 1);
    m_gamepadGrid.attach(m_padA, 1, 1, 1, 1);
    m_gamepadGrid.attach(m_padB, 2, 1, 1, 1);
    m_gamepadGrid.attach(m_padLB, 0, 0, 1, 1);
    m_gamepadGrid.attach(m_padRB, 2, 0, 1, 1);
    m_gamepadGrid.attach(m_padLeftStick, 0, 2, 2, 1);
    m_gamepadGrid.attach(m_padRightStick, 2, 2, 2, 1);
    m_gamepadFrame.add(m_gamepadGrid);
    m_gamepadFrame.set_sensitive(m_gamepadAvailable);
    m_controlLayout.pack_start(m_gamepadFrame, Gtk::PACK_SHRINK);
    m_controlPanel.pack_start(m_controlLayout, Gtk::PACK_SHRINK);
    m_box.pack_start(m_controlPanel, Gtk::PACK_SHRINK);
    m_box.pack_start(m_workspace, Gtk::PACK_EXPAND_WIDGET);
    installControlCss();
    configureKeycap(m_keyW);
    configureKeycap(m_keyA);
    configureKeycap(m_keyS);
    configureKeycap(m_keyD);
    configureKeycap(m_keyQ);
    configureKeycap(m_keyE);
    configureKeycap(m_keyR);
    configureKeycap(m_keyF);
    configureKeycap(m_keyI);
    configureKeycap(m_keyT);
    configureKeycap(m_keyL);
    configureKeycap(m_keyV);
    configureKeycap(m_keyX);
    configureKeycap(m_padLeftStick);
    configureKeycap(m_padRightStick);
    configureKeycap(m_padA);
    configureKeycap(m_padB);
    configureKeycap(m_padX);
    configureKeycap(m_padY);
    configureKeycap(m_padLB);
    configureKeycap(m_padRB);
    m_controlPanel.hide();
    add(m_box);
    show_all_children();
    m_controlPanel.hide();
    if (auto* firstRow = m_vehicleList.get_row_at_index(0)) {
      m_vehicleList.select_row(*firstRow);
      updateVehicleRows();
    }
    refreshMapTile();

    m_start.signal_clicked().connect([this] {
      m_start.set_sensitive(false);
      m_stop.set_sensitive(false);
      m_runtime.startVideo();
    });
    m_stop.signal_clicked().connect([this] {
      m_stop.set_sensitive(false);
      m_acceptFrames = false;
      m_runtime.stopVideo();
    });
    m_arm.signal_clicked().connect([this] {
      sendSelectedFlightCommandIfReady("arm", {{"arm", "true"}});
    });
    m_takeoff.signal_clicked().connect([this] {
      sendSelectedFlightCommandIfReady("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
    });
    m_land.signal_clicked().connect([this] {
      sendSelectedFlightCommandIfReady("land");
    });
    m_emergencyStop.signal_clicked().connect([this] {
      if (m_controlMode) {
        setControlMode(false);
      }
      m_status.set_text("Emergency stop requested for Drone " + m_runtime.targetDroneId());
      m_runtime.sendMavlinkCommand("emergency_stop", {{"force_code", "21196"}});
    });
    m_patrol.signal_clicked().connect([this] {
      m_patrol.set_sensitive(false);
      m_status.set_text("Uploading cooperative patrol mission...");
      double centerLat = 35.1186;
      double centerLon = -89.9375;
      double sideMeters = 140.0;
      try {
        centerLat = std::stod(m_patrolLat.get_text());
        centerLon = std::stod(m_patrolLon.get_text());
        sideMeters = std::stod(m_patrolSizeMeters.get_text());
      }
      catch (const std::exception&) {
        m_status.set_text("Invalid patrol input; using University of Memphis fallback");
      }
      const auto routeWaypoints = m_planWaypoints;
      std::thread([this, centerLat, centerLon, sideMeters, routeWaypoints] {
        const bool ok = m_runtime.runPatrolCompensationTask(
          std::chrono::seconds(30), centerLat, centerLon, sideMeters, false, routeWaypoints);
        Glib::signal_idle().connect_once([this, ok] {
          m_status.set_text(ok ? "Patrol mission uploaded; arm/takeoff and start mission mode to fly it"
                               : "Patrol mission upload failed");
          m_patrol.set_sensitive(true);
          updateVehicleRows();
        });
      }).detach();
    });
    m_startMission.signal_clicked().connect([this] {
      m_status.set_text("Starting patrol mission by phase: arm all, take off all, start all...");
      m_startMission.set_sensitive(false);
      m_stopPatrol.set_sensitive(true);
      scheduleMissionStartPhase(0, 0);
    });
    m_stopPatrol.signal_clicked().connect([this] {
      m_status.set_text("Stopping patrol: landing patrol drones...");
      m_startMission.set_sensitive(false);
      m_stopPatrol.set_sensitive(false);
      schedulePatrolLandSequence(0);
    });
    m_controlToggle.signal_clicked().connect([this] {
      setControlMode(!m_controlMode);
      updateFlightControlControls();
    });
    m_refreshRecording.signal_clicked().connect([this] {
      m_runtime.requestRecordingManifest();
    });
    m_playRecording.signal_clicked().connect([this] {
      beginLocalStreamView();
      m_runtime.playLatestRecording();
    });
    m_keyboardMode.signal_toggled().connect([this] {
      if (!m_updatingInputMode && m_keyboardMode.get_active()) {
        setControlInputMode(false);
      }
    });
    m_gamepadMode.signal_toggled().connect([this] {
      if (!m_updatingInputMode && m_gamepadMode.get_active()) {
        setControlInputMode(true);
      }
    });
    m_mapZoomIn.signal_clicked().connect([this] {
      zoomMap(1);
    });
    m_mapZoomOut.signal_clicked().connect([this] {
      zoomMap(-1);
    });
    m_mapCenterGs.signal_clicked().connect([this] {
      centerMapOnGroundStation();
    });
    m_mapUndoWp.signal_clicked().connect([this] {
      undoMissionWaypoint();
    });
    m_mapClearWp.signal_clicked().connect([this] {
      clearMissionWaypoints();
    });
    signal_key_press_event().connect(
      [this](GdkEventKey* event) {
        if (event == nullptr) {
          return false;
        }
        return handleShortcutKeyPress(event->keyval);
      },
      false);
    signal_key_release_event().connect(
      [this](GdkEventKey* event) {
        if (event == nullptr) {
          return false;
        }
        return handleShortcutKeyRelease(event->keyval);
      },
      false);

    m_runtime.setStatusCallback([this](std::string status) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (status.rfind("Video packet stream", 0) == 0) {
          const auto droneId = statusField(status, "drone", "");
          if (droneId == m_runtime.targetDroneId()) {
            beginLocalStreamViewLocked();
          }
          updateVideoViewForSelectedLocked();
        }
        else if (status.rfind("Video stopped", 0) == 0) {
          updateVideoViewForSelectedLocked();
          status += ", decoded=" + std::to_string(m_decodedFrames.load());
        }
        else if (status.rfind("Timeout waiting for ", 0) == 0 ||
                 status.rfind("Video control response missing", 0) == 0 ||
                 status.rfind("NDNSF runtime not ready", 0) == 0 ||
                 status.rfind("No video streaming for selected drone ", 0) == 0 ||
                 status.rfind("Video already streaming drone=", 0) == 0) {
          updateVideoViewForSelectedLocked();
        }
        if (status.rfind("MAVLink ", 0) == 0 ||
            status.rfind("Telemetry ", 0) == 0) {
          const auto statusDrone = statusField(status, "drone", m_runtime.targetDroneId());
          const auto selectedDrone = m_runtime.targetDroneId();
          if (status.rfind("Telemetry ", 0) == 0) {
            const auto telemetry = m_runtime.telemetryForDrone(statusDrone);
            const auto mission = m_runtime.missionForDrone(statusDrone);
            const auto readiness = m_runtime.readinessForDrone(statusDrone);
            const auto video = m_runtime.videoForDrone(statusDrone);
            const auto command = m_runtime.commandForDrone(statusDrone);
            const auto safety = m_runtime.safetyForDrone(statusDrone);
            if (telemetry) {
              try {
                m_dronePositions[statusDrone] = {
                  std::stod(telemetry->lat), std::stod(telemetry->lon)
                };
                m_pendingMapRefresh = true;
              }
              catch (const std::exception&) {
              }
              if (statusDrone == selectedDrone) {
                m_pendingTelemetry = telemetry->statusLine() +
                  (readiness ? " " + readiness->statusLine() : "") +
                  (mission ? " " + mission->statusLine() : "") +
                  (video ? " " + video->statusLine() : "") +
                  (command ? " " + command->statusLine() : "") +
                  (safety ? " " + safety->statusLine() : "");
                m_pendingMap = mapTextForTelemetry(*telemetry, mission, selectedDrone,
                                                   readiness, video, command, safety);
              }
              m_pendingVehicleRowsRefresh = true;
            }
            else if (statusDrone == selectedDrone) {
              m_pendingTelemetry = status;
            }
          }
          else if (statusDrone == selectedDrone) {
            const auto telemetry = m_runtime.telemetryForDrone(statusDrone);
            const auto mission = m_runtime.missionForDrone(statusDrone);
            const auto readiness = m_runtime.readinessForDrone(statusDrone);
            const auto video = m_runtime.videoForDrone(statusDrone);
            const auto command = m_runtime.commandForDrone(statusDrone);
            const auto safety = m_runtime.safetyForDrone(statusDrone);
            m_pendingTelemetry = command ? command->statusLine() : status;
            if (telemetry) {
              m_pendingMap = mapTextForTelemetry(*telemetry, mission, selectedDrone,
                                                 readiness, video, command, safety);
            }
            m_pendingVehicleRowsRefresh = true;
          }
        }
        if (status.rfind("Link ", 0) == 0) {
          m_pendingLinkStatus = "Link RTT: " +
            statusField(status, "rtt_ms", "?") + " ms  " +
            statusField(status, "service", "unknown");
        }
        else {
          m_pendingStatus = std::move(status);
        }
      }
      m_statusDispatcher.emit();
    });
    m_runtime.setFrameCallback([this](std::vector<uint8_t> frame, uint64_t seq, uint64_t elapsedMs) {
      pushEncodedChunk(std::move(frame), seq, elapsedMs);
    });

    m_statusDispatcher.connect([this] {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (m_pendingButtonState) {
        m_start.set_sensitive(m_pendingStartSensitive);
        m_stop.set_sensitive(m_pendingStopSensitive);
        m_pendingButtonState = false;
      }
      if (m_pendingClearFrame) {
        m_image.clear();
        m_stats.set_text("Decoded frames: 0");
        m_pendingClearFrame = false;
      }
      m_status.set_text(m_pendingStatus);
      if (!m_pendingLinkStatus.empty()) {
        m_linkStatus.set_text(m_pendingLinkStatus);
      }
      if (!m_pendingTelemetry.empty()) {
        m_telemetry.set_text("Telemetry: " + m_pendingTelemetry);
      }
      if (m_pendingVehicleRowsRefresh) {
        updateVehicleRows();
        m_pendingVehicleRowsRefresh = false;
      }
      if (!m_pendingMap.empty()) {
        m_mapMission.set_text(m_pendingMap);
        refreshMapTile();
        m_pendingMap.clear();
        m_pendingMapRefresh = false;
      }
      else if (m_pendingMapRefresh) {
        refreshMapTile();
        m_pendingMapRefresh = false;
      }
    });
    m_gamepadDispatcher.connect([this] {
      updateGamepadVisualState();
    });
    m_frameDispatcher.connect([this] {
      Glib::RefPtr<Gdk::Pixbuf> pixbuf;
      uint64_t seq = 0;
      uint64_t elapsedMs = 0;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        pixbuf = m_pendingPixbuf;
        seq = m_pendingSeq;
        elapsedMs = m_pendingElapsedMs;
      }
      if (pixbuf) {
        m_image.set(pixbuf);
        m_stats.set_text("Decoded frames: " + std::to_string(m_decodedFrames.load()) +
                         "  latest chunk: " + std::to_string(seq) +
                         "  stream elapsed: " + std::to_string(elapsedMs) + " ms");
      }
    });
    Glib::signal_timeout().connect([this] {
      if (!m_droneIds.empty()) {
        const auto droneId = m_droneIds[m_telemetryPollIndex++ % m_droneIds.size()];
        m_runtime.requestTelemetryStatusForDrone(droneId);
      }
      else {
        m_runtime.requestTelemetryStatus();
      }
      return true;
    }, 1500);

    if (autoStart) {
      std::thread([this, autoStopSeconds, autoStartDelayMs] {
        std::this_thread::sleep_for(std::chrono::milliseconds(autoStartDelayMs));
        m_runtime.startVideo();
        for (int i = 0; i < 100 && !m_runtime.isStreaming(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::seconds(autoStopSeconds));
        m_runtime.stopVideo();
        if (m_autoRepeatStopTest) {
          std::this_thread::sleep_for(std::chrono::milliseconds(3500));
          m_runtime.stopVideo();
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }

    if (autoMavlinkTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_runtime.sendMavlinkCommand("arm", {{"arm", "true"}});
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
        std::this_thread::sleep_for(std::chrono::seconds(3));
        m_runtime.sendMavlinkCommand("land");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("emergency_stop", {{"force_code", "21196"}});
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }

    if (autoKeyboardTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
          handleShortcutKeyPress(GDK_KEY_i);
          handleShortcutKeyRelease(GDK_KEY_i);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_t);
          handleShortcutKeyRelease(GDK_KEY_t);
        });
        std::this_thread::sleep_for(std::chrono::seconds(8));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_l);
          handleShortcutKeyRelease(GDK_KEY_l);
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    if (autoManualControlTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
          handleShortcutKeyPress(GDK_KEY_i);
          handleShortcutKeyRelease(GDK_KEY_i);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_t);
          handleShortcutKeyRelease(GDK_KEY_t);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_w);
          handleShortcutKeyPress(GDK_KEY_r);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyRelease(GDK_KEY_w);
          handleShortcutKeyRelease(GDK_KEY_r);
        });
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_l);
          handleShortcutKeyRelease(GDK_KEY_l);
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    if (autoTwoDroneSwitchTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (m_droneIds.size() < 2) {
          Glib::signal_idle().connect_once([this] {
            m_status.set_text("Two-drone switch test needs at least two drones");
            hide();
          });
          return;
        }
        const auto first = m_droneIds[0];
        const auto second = m_droneIds[1];
        Glib::signal_idle().connect_once([this, second] {
          setControlMode(true);
          m_runtime.setTargetDroneId(second);
          updateVehicleRows();
          updateVideoViewForSelected();
          m_runtime.requestTelemetryStatus();
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("manual_control", {
          {"x", "500"}, {"y", "0"}, {"z", "520"}, {"r", "0"},
        });
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this, first] {
          m_runtime.setTargetDroneId(first);
          updateVehicleRows();
          updateVideoViewForSelected();
          m_runtime.requestTelemetryStatus();
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("manual_control", {
          {"x", "-500"}, {"y", "0"}, {"z", "520"}, {"r", "0"},
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    if (autoRecordingPlaybackTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        Glib::signal_idle().connect_once([this] {
          beginLocalStreamView();
          m_runtime.requestRecordingManifest();
          m_runtime.playLatestRecording();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
        while (std::chrono::steady_clock::now() < deadline &&
               m_decodedFrames.load() < 10) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    m_manualControlThread = std::thread([this] {
      runManualControlLoop();
    });
    if (m_gamepadAvailable) {
      m_gamepadThread = std::thread([this] {
        runGamepadLoop();
      });
    }
  }

  ~GroundStationWindow() override
  {
    m_acceptFrames = false;
    if (m_runtime.isStreaming()) {
      m_runtime.stopVideo();
    }
    m_controlMode = false;
    m_manualActive = false;
    m_manualControlDone = true;
    m_gamepadDone = true;
    if (m_manualControlThread.joinable()) {
      m_manualControlThread.join();
    }
    if (m_gamepadThread.joinable()) {
      m_gamepadThread.join();
    }
  }

private:
  bool
  selectedReadinessAllows(const std::string& action, std::string& reason) const
  {
    const auto readiness = m_runtime.readinessForDrone(m_runtime.targetDroneId());
    if (!readiness) {
      reason = "no-telemetry";
      return false;
    }
    if (action == "arm") {
      if (readiness->armed == "true") {
        reason = "already-armed";
        return false;
      }
      if (!readiness->readyForArm()) {
        reason = readiness->readinessReason;
        return false;
      }
      reason = "ok";
      return true;
    }
    if (action == "takeoff") {
      if (!readiness->readyForTakeoff()) {
        reason = readiness->readyForArm() ? "not-armed" : readiness->readinessReason;
        return false;
      }
      reason = "ok";
      return true;
    }
    if (action == "land") {
      if (!readiness->readyForLand()) {
        reason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
        return false;
      }
      reason = "ok";
      return true;
    }
    if (action == "manual_control") {
      if (!readiness->readyForManualControl()) {
        reason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
        return false;
      }
      reason = "ok";
      return true;
    }
    if (action == "control_panel") {
      if (readiness->readyForArm() || readiness->readyForManualControl() || readiness->readyForLand()) {
        reason = "ok";
        return true;
      }
      reason = readiness->readinessReason;
      return false;
    }
    reason = "ok";
    return true;
  }

  bool
  sendSelectedFlightCommandIfReady(const std::string& commandName, Fields params = {})
  {
    std::string reason;
    if (!selectedReadinessAllows(commandName, reason)) {
      m_status.set_text("Flight command blocked: " + commandName +
                        " drone=" + m_runtime.targetDroneId() +
                        " reason=" + reason);
      updateFlightControlControls();
      return false;
    }
    if (commandName == "land" && m_controlMode) {
      setControlMode(false);
    }
    return m_runtime.sendMavlinkCommand(commandName, std::move(params));
  }

  void
  updateFlightControlControls()
  {
    std::string reason;
    const bool armOk = selectedReadinessAllows("arm", reason);
    const bool takeoffOk = selectedReadinessAllows("takeoff", reason);
    const bool landOk = selectedReadinessAllows("land", reason);
    const bool manualOk = selectedReadinessAllows("manual_control", reason);
    const bool panelOk = selectedReadinessAllows("control_panel", reason);
    m_arm.set_sensitive(armOk);
    m_takeoff.set_sensitive(takeoffOk);
    m_land.set_sensitive(landOk);
    if (m_controlMode && !panelOk) {
      setControlMode(false);
      m_status.set_text("Manual control disabled: drone=" + m_runtime.targetDroneId() +
                        " reason=" + reason);
    }
    m_controlToggle.set_sensitive(panelOk || m_controlMode || manualOk);
  }

  bool
  handleShortcutKeyPress(guint keyval)
  {
    if (!m_controlMode || m_useGamepad.load()) {
      return false;
    }
    const auto normalized = normalizeShortcutKey(keyval);
    if (normalized == 0) {
      return false;
    }
    setKeyPressed(normalized, true);
    if (!m_pressedKeys.insert(normalized).second) {
      return true;
    }

    switch (normalized) {
    case GDK_KEY_w:
    case GDK_KEY_a:
    case GDK_KEY_s:
    case GDK_KEY_d:
    case GDK_KEY_q:
    case GDK_KEY_e:
    case GDK_KEY_r:
    case GDK_KEY_f:
      updateManualAxisState();
      return true;
    case GDK_KEY_i:
      sendSelectedFlightCommandIfReady("arm", {{"arm", "true"}});
      return true;
    case GDK_KEY_t:
      sendSelectedFlightCommandIfReady("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
      return true;
    case GDK_KEY_l:
      sendSelectedFlightCommandIfReady("land");
      return true;
    case GDK_KEY_v:
      if (!isSelectedDroneVideoStreaming()) {
        m_start.set_sensitive(false);
        m_stop.set_sensitive(false);
        m_runtime.startVideo();
      }
      return true;
    case GDK_KEY_x:
      if (isSelectedDroneVideoStreaming()) {
        m_stop.set_sensitive(false);
        m_acceptFrames = false;
        m_runtime.stopVideo();
      }
      return true;
    default:
      return false;
    }
  }

  bool
  handleShortcutKeyRelease(guint keyval)
  {
    if (m_useGamepad.load()) {
      return false;
    }
    const auto normalized = normalizeShortcutKey(keyval);
    if (normalized == 0) {
      return false;
    }
    m_pressedKeys.erase(normalized);
    setKeyPressed(normalized, false);
    updateManualAxisState();
    return m_controlMode;
  }

  guint
  normalizeShortcutKey(guint keyval) const
  {
    switch (keyval) {
    case GDK_KEY_w:
    case GDK_KEY_W:
      return GDK_KEY_w;
    case GDK_KEY_a:
    case GDK_KEY_A:
      return GDK_KEY_a;
    case GDK_KEY_s:
    case GDK_KEY_S:
      return GDK_KEY_s;
    case GDK_KEY_d:
    case GDK_KEY_D:
      return GDK_KEY_d;
    case GDK_KEY_q:
    case GDK_KEY_Q:
      return GDK_KEY_q;
    case GDK_KEY_e:
    case GDK_KEY_E:
      return GDK_KEY_e;
    case GDK_KEY_r:
    case GDK_KEY_R:
      return GDK_KEY_r;
    case GDK_KEY_f:
    case GDK_KEY_F:
      return GDK_KEY_f;
    case GDK_KEY_i:
    case GDK_KEY_I:
      return GDK_KEY_i;
    case GDK_KEY_t:
    case GDK_KEY_T:
      return GDK_KEY_t;
    case GDK_KEY_l:
    case GDK_KEY_L:
      return GDK_KEY_l;
    case GDK_KEY_v:
    case GDK_KEY_V:
      return GDK_KEY_v;
    case GDK_KEY_x:
    case GDK_KEY_X:
      return GDK_KEY_x;
    default:
      return 0;
    }
  }

  void
  setControlMode(bool enabled)
  {
    if (enabled) {
      std::string reason;
      if (!selectedReadinessAllows("control_panel", reason)) {
        m_status.set_text("Manual control blocked: drone=" + m_runtime.targetDroneId() +
                          " reason=" + reason);
        updateFlightControlControls();
        return;
      }
    }
    m_controlMode = enabled;
    m_controlToggle.set_label(enabled ? "Stop Control" : "Start Control");
    if (enabled) {
      m_controlPanel.show();
      grab_focus();
    }
    else {
      m_pressedKeys.clear();
      m_manualX = 0;
      m_manualY = 0;
      m_manualZ = 500;
      m_manualR = 0;
      m_manualActive = false;
      sendManualControlOnce();
      setKeyPressed(GDK_KEY_w, false);
      setKeyPressed(GDK_KEY_a, false);
      setKeyPressed(GDK_KEY_s, false);
      setKeyPressed(GDK_KEY_d, false);
      setKeyPressed(GDK_KEY_q, false);
      setKeyPressed(GDK_KEY_e, false);
      setKeyPressed(GDK_KEY_r, false);
      setKeyPressed(GDK_KEY_f, false);
      setKeyPressed(GDK_KEY_i, false);
      setKeyPressed(GDK_KEY_t, false);
      setKeyPressed(GDK_KEY_l, false);
      setKeyPressed(GDK_KEY_v, false);
      setKeyPressed(GDK_KEY_x, false);
      updateGamepadVisualState();
      m_controlPanel.hide();
    }
  }

  void
  setControlInputMode(bool useGamepad)
  {
    if (useGamepad && !m_gamepadAvailable) {
      useGamepad = false;
    }
    m_useGamepad = useGamepad;
    m_updatingInputMode = true;
    m_keyboardMode.set_active(!useGamepad);
    m_gamepadMode.set_active(useGamepad);
    m_updatingInputMode = false;

    m_pressedKeys.clear();
    m_manualX = 0;
    m_manualY = 0;
    m_manualZ = 500;
    m_manualR = 0;
    m_manualActive = false;
    resetKeyboardVisualState();
    updateGamepadManualState();
    updateGamepadVisualState();
    m_controlHelp.set_text(useGamepad ?
      "Xbox Gamepad: left stick yaw/throttle, right stick roll/pitch, A arm, Y takeoff, B land, X video." :
      "Keyboard: left pad A/D yaw and R/F throttle; right pad Q/E roll and W/S pitch; I/T/L/V/X commands.");
  }

  void
  configureKeycap(Gtk::Button& button)
  {
    button.set_sensitive(false);
    button.get_style_context()->add_class("uav-keycap");
  }

  void
  installControlCss()
  {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(
      ".uav-keycap {"
      "  color: #202124;"
      "  background: #f3f4f6;"
      "  border: 1px solid #9aa0a6;"
      "  border-radius: 4px;"
      "  padding: 6px 10px;"
      "}"
      ".uav-keycap-active {"
      "  color: white;"
      "  background: #111111;"
      "  border: 1px solid #111111;"
      "}"
    );
    auto screen = Gdk::Screen::get_default();
    if (screen) {
      Gtk::StyleContext::add_provider_for_screen(
        screen, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
  }

  void
  setKeyPressed(guint keyval, bool pressed)
  {
    Gtk::Button* button = nullptr;
    switch (keyval) {
    case GDK_KEY_w:
      button = &m_keyW;
      break;
    case GDK_KEY_a:
      button = &m_keyA;
      break;
    case GDK_KEY_s:
      button = &m_keyS;
      break;
    case GDK_KEY_d:
      button = &m_keyD;
      break;
    case GDK_KEY_q:
      button = &m_keyQ;
      break;
    case GDK_KEY_e:
      button = &m_keyE;
      break;
    case GDK_KEY_r:
      button = &m_keyR;
      break;
    case GDK_KEY_f:
      button = &m_keyF;
      break;
    case GDK_KEY_i:
      button = &m_keyI;
      break;
    case GDK_KEY_t:
      button = &m_keyT;
      break;
    case GDK_KEY_l:
      button = &m_keyL;
      break;
    case GDK_KEY_v:
      button = &m_keyV;
      break;
    case GDK_KEY_x:
      button = &m_keyX;
      break;
    default:
      return;
    }
    auto context = button->get_style_context();
    if (pressed) {
      context->add_class("uav-keycap-active");
    }
    else {
      context->remove_class("uav-keycap-active");
    }
  }

  void
  setButtonActive(Gtk::Button& button, bool active)
  {
    auto context = button.get_style_context();
    if (active) {
      context->add_class("uav-keycap-active");
    }
    else {
      context->remove_class("uav-keycap-active");
    }
  }

  void
  resetKeyboardVisualState()
  {
    setKeyPressed(GDK_KEY_w, false);
    setKeyPressed(GDK_KEY_a, false);
    setKeyPressed(GDK_KEY_s, false);
    setKeyPressed(GDK_KEY_d, false);
    setKeyPressed(GDK_KEY_q, false);
    setKeyPressed(GDK_KEY_e, false);
    setKeyPressed(GDK_KEY_r, false);
    setKeyPressed(GDK_KEY_f, false);
    setKeyPressed(GDK_KEY_i, false);
    setKeyPressed(GDK_KEY_t, false);
    setKeyPressed(GDK_KEY_l, false);
    setKeyPressed(GDK_KEY_v, false);
    setKeyPressed(GDK_KEY_x, false);
  }

  void
  updateManualAxisState()
  {
    auto has = [this](guint key) {
      return m_pressedKeys.find(key) != m_pressedKeys.end();
    };
    const int x = (has(GDK_KEY_w) ? 650 : 0) + (has(GDK_KEY_s) ? -650 : 0);
    const int y = (has(GDK_KEY_e) ? 500 : 0) + (has(GDK_KEY_q) ? -500 : 0);
    const int r = (has(GDK_KEY_d) ? 550 : 0) + (has(GDK_KEY_a) ? -550 : 0);
    const int z = 500 + (has(GDK_KEY_r) ? 250 : 0) + (has(GDK_KEY_f) ? -250 : 0);
    m_manualX = std::clamp(x, -1000, 1000);
    m_manualY = std::clamp(y, -1000, 1000);
    m_manualZ = std::clamp(z, 0, 1000);
    m_manualR = std::clamp(r, -1000, 1000);
    m_manualActive = has(GDK_KEY_w) || has(GDK_KEY_a) || has(GDK_KEY_s) ||
                     has(GDK_KEY_d) || has(GDK_KEY_q) || has(GDK_KEY_e) ||
                     has(GDK_KEY_r) || has(GDK_KEY_f);
  }

  void
  updateGamepadManualState()
  {
    if (!m_useGamepad.load()) {
      return;
    }
    const int leftX = scaleJoystickAxis(m_gamepadAxes[0].load());
    const int leftY = scaleJoystickAxis(m_gamepadAxes[1].load());
    const int rightX = scaleJoystickAxis(m_gamepadAxes[3].load());
    const int rightY = scaleJoystickAxis(m_gamepadAxes[4].load());
    m_manualR = std::clamp(leftX * 550 / 1000, -1000, 1000);
    m_manualZ = std::clamp(500 - leftY * 500 / 1000, 0, 1000);
    m_manualY = std::clamp(rightX * 500 / 1000, -1000, 1000);
    m_manualX = std::clamp(-rightY * 650 / 1000, -1000, 1000);
    m_manualActive = leftX != 0 || leftY != 0 || rightX != 0 || rightY != 0;
  }

  void
  updateGamepadVisualState()
  {
    const bool useGamepad = m_controlMode && m_useGamepad.load();
    const int leftX = scaleJoystickAxis(m_gamepadAxes[0].load());
    const int leftY = scaleJoystickAxis(m_gamepadAxes[1].load());
    const int rightX = scaleJoystickAxis(m_gamepadAxes[3].load());
    const int rightY = scaleJoystickAxis(m_gamepadAxes[4].load());
    setButtonActive(m_padLeftStick, useGamepad && (leftX != 0 || leftY != 0));
    setButtonActive(m_padRightStick, useGamepad && (rightX != 0 || rightY != 0));
    setButtonActive(m_padA, useGamepad && m_gamepadButtons[0].load());
    setButtonActive(m_padB, useGamepad && m_gamepadButtons[1].load());
    setButtonActive(m_padX, useGamepad && m_gamepadButtons[2].load());
    setButtonActive(m_padY, useGamepad && m_gamepadButtons[3].load());
    setButtonActive(m_padLB, useGamepad && m_gamepadButtons[4].load());
    setButtonActive(m_padRB, useGamepad && m_gamepadButtons[5].load());
  }

  void
  handleGamepadButtonPress(uint8_t button)
  {
    if (!m_controlMode || !m_useGamepad.load()) {
      return;
    }
    Glib::signal_idle().connect_once([this, button] {
      if (!m_controlMode || !m_useGamepad.load()) {
        return;
      }
      switch (button) {
      case 0: // Xbox A
        sendSelectedFlightCommandIfReady("arm", {{"arm", "true"}});
        break;
      case 1: // Xbox B
        sendSelectedFlightCommandIfReady("land");
        break;
      case 2: // Xbox X
        if (isSelectedDroneVideoStreaming()) {
          m_runtime.stopVideo();
        }
        else {
          m_runtime.startVideo();
        }
        break;
      case 3: // Xbox Y
        sendSelectedFlightCommandIfReady("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
        break;
      default:
        break;
      }
    });
  }

  void
  runGamepadLoop()
  {
    while (!m_gamepadDone.load()) {
      const int fd = open(m_gamepadDevicePath.c_str(), O_RDONLY | O_NONBLOCK);
      if (fd < 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      while (!m_gamepadDone.load()) {
        js_event event{};
        const auto n = read(fd, &event, sizeof(event));
        if (n == static_cast<ssize_t>(sizeof(event))) {
          event.type &= ~JS_EVENT_INIT;
          if (event.type == JS_EVENT_AXIS && event.number < m_gamepadAxes.size()) {
            m_gamepadAxes[event.number] = event.value;
            updateGamepadManualState();
            m_gamepadDispatcher.emit();
          }
          else if (event.type == JS_EVENT_BUTTON && event.number < m_gamepadButtons.size()) {
            m_gamepadButtons[event.number] = event.value != 0;
            if (event.value != 0) {
              handleGamepadButtonPress(event.number);
            }
            m_gamepadDispatcher.emit();
          }
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        else if (n < 0 && errno == EINTR) {
          continue;
        }
        else {
          break;
        }
      }
      ::close(fd);
    }
  }

  void
  runManualControlLoop()
  {
    while (!m_manualControlDone.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (!m_controlMode) {
        continue;
      }
      if (m_useGamepad.load()) {
        updateGamepadManualState();
      }
      sendManualControlOnce();
    }
  }

  void
  sendManualControlOnce()
  {
    std::string reason;
    if (!selectedReadinessAllows("manual_control", reason)) {
      return;
    }
    m_runtime.sendMavlinkCommand("manual_control", {
      {"x", std::to_string(m_manualX.load())},
      {"y", std::to_string(m_manualY.load())},
      {"z", std::to_string(m_manualZ.load())},
      {"r", std::to_string(m_manualR.load())},
      {"buttons", "0"},
    });
  }

  void
  setPendingButtonStateLocked(bool startSensitive, bool stopSensitive)
  {
    m_pendingButtonState = true;
    m_pendingStartSensitive = startSensitive;
    m_pendingStopSensitive = stopSensitive;
  }

  void
  beginLocalStreamView()
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    beginLocalStreamViewLocked();
  }

  void
  beginLocalStreamViewLocked()
  {
    ++m_streamGeneration;
    m_acceptFrames = true;
    m_decodedFrames = 0;
    m_pendingSeq = 0;
    m_pendingElapsedMs = 0;
    m_pendingPixbuf.reset();
  }

  void
  updateVideoViewForSelected()
  {
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      updateVideoViewForSelectedLocked();
    }
    m_statusDispatcher.emit();
  }

  void
  updateVideoViewForSelectedLocked()
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    const bool selectedRemoteStreaming = isVideoStateStreamingForDrone(selectedDrone);
    const bool selectedDisplayActive = m_runtime.isVideoDisplayActiveForDrone(selectedDrone);
    setPendingButtonStateLocked(!selectedRemoteStreaming && !selectedDisplayActive,
                                selectedRemoteStreaming || selectedDisplayActive);
    if (selectedDisplayActive) {
      if (!m_acceptFrames) {
        beginLocalStreamViewLocked();
      }
      m_pendingClearFrame = false;
      return;
    }

    ++m_streamGeneration;
    m_acceptFrames = false;
    m_pendingPixbuf.reset();
    m_pendingSeq = 0;
    m_pendingElapsedMs = 0;
    m_decodedFrames = 0;
    m_pendingClearFrame = true;
  }

  bool
  isVideoStateStreamingForDrone(const std::string& droneId) const
  {
    const auto video = m_runtime.videoForDrone(droneId);
    return video && video->isStreaming();
  }

  bool
  isSelectedDroneVideoStreaming() const
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    return isVideoStateStreamingForDrone(selectedDrone) ||
           m_runtime.isStreamingForDrone(selectedDrone);
  }

  void
  pushEncodedChunk(std::vector<uint8_t> chunk, uint64_t seq, uint64_t elapsedMs)
  {
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_acceptFrames || !m_runtime.isVideoDisplayActiveForDrone(m_runtime.targetDroneId())) {
        return;
      }
      generation = m_streamGeneration;
    }
    Glib::signal_idle().connect_once([this, chunk = std::move(chunk), seq, elapsedMs, generation] {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_acceptFrames ||
            generation != m_streamGeneration ||
            !m_runtime.isVideoDisplayActiveForDrone(m_runtime.targetDroneId())) {
          return;
        }
      }
      auto loader = Gdk::PixbufLoader::create();
      try {
        loader->write(chunk.data(), chunk.size());
        loader->close();
        auto pixbuf = loader->get_pixbuf();
        if (pixbuf) {
          {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_pendingPixbuf = pixbuf;
            m_pendingSeq = seq;
            m_pendingElapsedMs = elapsedMs;
          }
          ++m_decodedFrames;
          if (m_decodedFrames.load() <= 3 || m_decodedFrames.load() % 30 == 0) {
            std::cout << "GS_DECODED_FRAMES count=" << m_decodedFrames.load() << std::endl;
          }
          m_frameDispatcher.emit();
        }
      }
      catch (const Glib::Error& e) {
        NDN_LOG_WARN("GS_DECODER_ERROR " << e.what());
        std::cout << "GS_DECODER_ERROR " << e.what() << std::endl;
      }
    });
  }

  void
  updateVehicleRows()
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      auto* row = m_vehicleList.get_row_at_index(static_cast<int>(i));
      if (row == nullptr) {
        continue;
      }
      auto* label = dynamic_cast<Gtk::Label*>(row->get_child());
      if (label == nullptr) {
        continue;
      }
      const bool selected = m_droneIds[i] == selectedDrone;
      const auto telemetry = m_runtime.telemetryForDrone(m_droneIds[i]);
      const auto readiness = m_runtime.readinessForDrone(m_droneIds[i]);
      const auto mission = m_runtime.missionForDrone(m_droneIds[i]);
      const auto video = m_runtime.videoForDrone(m_droneIds[i]);
      const auto command = m_runtime.commandForDrone(m_droneIds[i]);
      const auto safety = m_runtime.safetyForDrone(m_droneIds[i]);
      std::string rowText = std::string(selected ? "● " : "○ ") + "Drone " + m_droneIds[i] +
                            (selected ? " active" : " standby");
      if (readiness) {
        rowText += " " + readiness->readiness;
        rowText += " armed=" + readiness->armed;
        rowText += " gps=" + readiness->gpsReady;
      }
      else if (telemetry) {
        rowText += " " + telemetry->readiness;
        rowText += " armed=" + telemetry->armed;
        rowText += " gps=" + telemetry->gpsFixName;
      }
      if (telemetry) {
        rowText += " bat=" + telemetry->batteryPercent + "%";
      }
      if (mission && !mission->isIdle()) {
        rowText += " mission=" + mission->phase;
      }
      if (video && video->status != "unknown") {
        rowText += " video=" + video->status;
      }
      if (command && command->command != "none") {
        rowText += " cmd=" + command->command + ":" + command->ackResult;
      }
      if (safety) {
        rowText += " safe=" + safety->manualControlState + "/" + safety->linkState;
      }
      label->set_text(rowText);
    }
    const auto telemetry = m_runtime.telemetryForDrone(selectedDrone);
    const auto mission = m_runtime.missionForDrone(selectedDrone);
    const auto readiness = m_runtime.readinessForDrone(selectedDrone);
    const auto video = m_runtime.videoForDrone(selectedDrone);
    const auto command = m_runtime.commandForDrone(selectedDrone);
    const auto safety = m_runtime.safetyForDrone(selectedDrone);
    if (telemetry) {
      m_mapMission.set_text(mapTextForTelemetry(*telemetry, mission, selectedDrone,
                                                readiness, video, command, safety));
    }
    else {
      m_mapMission.set_text("Map / mission workspace\n\n"
                            "GS center: University of Memphis\n"
                            "Selected drone: " + selectedDrone + "\n"
                            "Map markers show GS, drones, and mission waypoints.\n"
                            "Click map to append waypoints, then upload/start the mission.");
    }
    m_services.set_text(m_runtime.serviceCatalogForDrone(selectedDrone));
    updateMissionControls();
    updateFlightControlControls();
    refreshMapTile();
  }

  void
  updateMissionControls()
  {
    const auto startableDrones = m_runtime.missionStartableDrones();
    bool hasUploaded = !startableDrones.empty();
    bool hasExecuting = false;
    bool hasStopping = false;
    for (const auto& droneId : m_droneIds) {
      const auto mission = m_runtime.missionForDrone(droneId);
      if (!mission) {
        continue;
      }
      hasUploaded = hasUploaded || mission->isStartable();
      hasExecuting = hasExecuting || mission->isExecuting();
      hasStopping = hasStopping || mission->isStopping();
    }
    m_startMission.set_sensitive(hasUploaded && !hasExecuting && !hasStopping);
    m_stopPatrol.set_sensitive(hasUploaded || hasExecuting || hasStopping);
  }

  static std::string
  statusField(const std::string& status, const std::string& key,
              const std::string& fallback = "unknown")
  {
    const auto marker = key + "=";
    const auto start = status.find(marker);
    if (start == std::string::npos) {
      return fallback;
    }
    const auto valueStart = start + marker.size();
    const auto valueEnd = status.find(' ', valueStart);
    return status.substr(valueStart, valueEnd == std::string::npos ?
                         std::string::npos : valueEnd - valueStart);
  }

  static std::string
  mapTextForTelemetry(const TelemetryState& telemetry,
                      const std::optional<MissionState>& mission,
                      const std::string& selectedDrone,
                      const std::optional<ReadinessState>& readiness = std::nullopt,
                      const std::optional<VideoState>& video = std::nullopt,
                      const std::optional<FlightCommandState>& command = std::nullopt,
                      const std::optional<SafetyState>& safety = std::nullopt)
  {
    const auto missionPhase = mission ? mission->phase : "idle";
    const auto missionDetail = mission ? mission->detail : "idle";
    std::string text = telemetry.mapSummary(selectedDrone) + "\n"
      "Mission: " + missionPhase + " (" + missionDetail + ")";
    if (mission) {
      text += "\nMission model: start=" + std::string(mission->isStartable() ? "ready" : "blocked") +
              " stop=" + std::string(mission->isStoppable() ? "ready" : "blocked") +
              " busy=" + std::string(mission->isBusyForAssignment() ? "yes" : "no") +
              " terminal=" + std::string(mission->isTerminal() ? "yes" : "no");
    }
    if (readiness) {
      text += "\nReadiness model: " + readiness->readiness +
              " reason=" + readiness->readinessReason +
              " arm=" + (readiness->readyForArm() ? "ready" : "blocked") +
              " takeoff=" + (readiness->readyForTakeoff() ? "ready" : "blocked") +
              " land=" + (readiness->readyForLand() ? "ready" : "blocked") +
              " manual=" + (readiness->readyForManualControl() ? "ready" : "blocked");
    }
    if (video) {
      text += "\nVideo model: " + video->status +
              " capture=" + video->capture +
              " recording=" + video->recording +
              " stream=" + video->streamId +
              " packets=" + std::to_string(video->streamPacketsPublished) +
              " decoded=" + std::to_string(video->decodedFrames);
    }
    if (command && command->command != "none") {
      text += "\nCommand model: " + command->command +
              " accepted=" + command->accepted +
              " ack=" + command->ackResult +
              " state=" + command->flightControllerState +
              " safety=" + std::string(command->isSafetyCritical() ? "yes" : "no") +
              " detail=" + command->detail;
    }
    if (safety) {
      text += "\nSafety model: link=" + safety->linkState +
              " manual=" + safety->manualControlState +
              " replay=" + safety->manualReplayActive +
              " neutral=" + safety->manualNeutralSent +
              " fresh_for=" + std::to_string(safety->manualFreshForMs) + "ms" +
              " attention=" + std::string(safety->needsOperatorAttention() ? "yes" : "no") +
              " detail=" + safety->detail;
    }
    return text;
  }

  struct MapMarker
  {
    std::string label;
    double lat = 35.1186;
    double lon = -89.9375;
    uint8_t r = 220;
    uint8_t g = 20;
    uint8_t b = 40;
  };

  static std::pair<double, double>
  tileFloatForLatLon(double lat, double lon, int zoom)
  {
    const auto latRad = lat * M_PI / 180.0;
    const auto n = std::pow(2.0, zoom);
    return {
      (lon + 180.0) / 360.0 * n,
      (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n
    };
  }

  static std::pair<double, double>
  latLonForTilePixel(int tileX, int tileY, int zoom, double pixelX, double pixelY)
  {
    const auto n = std::pow(2.0, zoom);
    const auto x = (static_cast<double>(tileX) + pixelX / 256.0) / n;
    const auto y = (static_cast<double>(tileY) + pixelY / 256.0) / n;
    const auto lon = x * 360.0 - 180.0;
    const auto latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y)));
    return {latRad * 180.0 / M_PI, lon};
  }

  static std::pair<double, double>
  latLonForTileFloat(double tileX, double tileY, int zoom)
  {
    const auto n = std::pow(2.0, zoom);
    const auto lon = tileX / n * 360.0 - 180.0;
    const auto latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * tileY / n)));
    return {latRad * 180.0 / M_PI, lon};
  }

  static void
  drawTinyText(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf, int x, int y,
               const std::string& text, uint8_t r, uint8_t g, uint8_t b)
  {
    if (!pixbuf) {
      return;
    }
    const int width = pixbuf->get_width();
    const int height = pixbuf->get_height();
    const int channels = pixbuf->get_n_channels();
    const int rowstride = pixbuf->get_rowstride();
    auto* pixels = pixbuf->get_pixels();
    if (pixels == nullptr || channels < 3) {
      return;
    }

    auto setPixel = [&] (int x, int y, uint8_t r, uint8_t g, uint8_t b) {
      if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
      }
      auto* p = pixels + y * rowstride + x * channels;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      if (channels >= 4) {
        p[3] = 255;
      }
    };

    auto patternFor = [] (char ch) -> std::array<const char*, 7> {
      switch (ch) {
      case 'A': return {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
      case 'B': return {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
      case 'C': return {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
      case 'D': return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
      case 'E': return {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
      case 'F': return {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
      case 'G': return {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
      case 'I': return {"01110", "00100", "00100", "00100", "00100", "00100", "01110"};
      case 'L': return {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
      case 'N': return {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
      case 'O': return {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
      case 'P': return {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
      case 'R': return {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
      case 'S': return {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
      case 'T': return {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
      case 'U': return {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
      case 'W': return {"10001", "10001", "10001", "10101", "10101", "11011", "10001"};
      case 'X': return {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
      case '0': return {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
      case '1': return {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
      case '2': return {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
      case '3': return {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
      case '4': return {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
      case '5': return {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
      case '6': return {"01110", "10000", "10000", "11110", "10001", "10001", "01110"};
      case '7': return {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
      case '8': return {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
      case '9': return {"01110", "10001", "10001", "01111", "00001", "00001", "01110"};
      default: return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
      }
    };

    int cursor = x;
    for (char ch : text) {
      if (ch >= 'a' && ch <= 'z') {
        ch = static_cast<char>(ch - 'a' + 'A');
      }
      if (ch == ' ') {
        cursor += 4;
        continue;
      }
      const auto pattern = patternFor(ch);
      for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
          if (pattern[row][col] == '1') {
            setPixel(cursor + col, y + row, r, g, b);
          }
        }
      }
      cursor += 6;
    }
  }

  static void
  drawMapMarker(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf, int cx, int cy,
                const std::string& label, uint8_t r, uint8_t g, uint8_t b)
  {
    if (!pixbuf) {
      return;
    }
    const int width = pixbuf->get_width();
    const int height = pixbuf->get_height();
    const int channels = pixbuf->get_n_channels();
    const int rowstride = pixbuf->get_rowstride();
    auto* pixels = pixbuf->get_pixels();
    if (pixels == nullptr || channels < 3) {
      return;
    }

    auto setPixel = [&] (int x, int y, uint8_t r, uint8_t g, uint8_t b) {
      if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
      }
      auto* p = pixels + y * rowstride + x * channels;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      if (channels >= 4) {
        p[3] = 255;
      }
    };

    for (int dy = -8; dy <= 8; ++dy) {
      for (int dx = -8; dx <= 8; ++dx) {
        const auto dist2 = dx * dx + dy * dy;
        if (dist2 <= 64 && dist2 >= 36) {
          setPixel(cx + dx, cy + dy, 255, 255, 255);
        }
        if (std::abs(dx) <= 1 || std::abs(dy) <= 1) {
          setPixel(cx + dx, cy + dy, r, g, b);
        }
      }
    }
    for (int d = -4; d <= 4; ++d) {
      setPixel(cx + d, cy + d, r, g, b);
      setPixel(cx + d, cy - d, r, g, b);
    }
    drawTinyText(pixbuf, std::clamp(cx + 10, 0, width - 24),
                 std::clamp(cy - 4, 0, height - 8), label, r, g, b);
  }

  std::vector<MapMarker>
  currentMapMarkers() const
  {
    std::vector<MapMarker> markers;
    markers.push_back({"GS", m_groundStationLat, m_groundStationLon, 20, 80, 220});
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      const auto& droneId = m_droneIds[i];
      const auto found = m_dronePositions.find(droneId);
      double lat = m_groundStationLat;
      double lon = m_groundStationLon + (static_cast<double>(i) + 1.0) * 0.0007;
      if (found != m_dronePositions.end()) {
        lat = found->second.first;
        lon = found->second.second;
      }
      if (std::abs(lat - m_groundStationLat) < 0.00001 &&
          std::abs(lon - m_groundStationLon) < 0.00001) {
        lon += (static_cast<double>(i) + 1.0) * 0.00055;
      }
      std::string label = droneId;
      auto r = static_cast<uint8_t>(i == 0 ? 220 : 20);
      auto g = static_cast<uint8_t>(i == 0 ? 40 : 160);
      auto b = static_cast<uint8_t>(i == 0 ? 40 : 70);
      if (const auto mission = m_runtime.missionForDrone(droneId)) {
        if (mission->isUploading() || mission->isUploaded()) {
          label += " U";
          r = 245;
          g = 160;
          b = 20;
        }
        else if (mission->isExecuting()) {
          label += " R";
          r = 30;
          g = 160;
          b = 80;
        }
        else if (mission->isStopping()) {
          label += " S";
          r = 70;
          g = 110;
          b = 220;
        }
        else if (mission->isCompleted()) {
          label += " C";
          r = 20;
          g = 150;
          b = 180;
        }
        else if (mission->isFailed() || mission->isCancelled()) {
          label += " X";
          r = 220;
          g = 30;
          b = 40;
        }
      }
      if (const auto safety = m_runtime.safetyForDrone(droneId);
          safety && safety->needsOperatorAttention()) {
        label += " !";
        r = 220;
        g = 30;
        b = 40;
      }
      markers.push_back({label, lat, lon, r, g, b});
    }
    if (!m_planWaypoints.empty()) {
      for (size_t i = 0; i < m_planWaypoints.size(); ++i) {
        markers.push_back({"WP" + std::to_string(i + 1),
                           m_planWaypoints[i].first, m_planWaypoints[i].second,
                           245, 160, 20});
      }
    }
    else if (m_hasPatrolCenter) {
      markers.push_back({"WP", m_patrolCenterLat, m_patrolCenterLon, 245, 160, 20});
    }
    return markers;
  }

  void
  updatePatrolInputsFromWaypoints()
  {
    if (m_planWaypoints.empty()) {
      m_hasPatrolCenter = false;
      return;
    }
    double latSum = 0.0;
    double lonSum = 0.0;
    for (const auto& point : m_planWaypoints) {
      latSum += point.first;
      lonSum += point.second;
    }
    m_patrolCenterLat = latSum / static_cast<double>(m_planWaypoints.size());
    m_patrolCenterLon = lonSum / static_cast<double>(m_planWaypoints.size());
    m_hasPatrolCenter = true;
    std::ostringstream latText;
    std::ostringstream lonText;
    latText << std::fixed << std::setprecision(6) << m_patrolCenterLat;
    lonText << std::fixed << std::setprecision(6) << m_patrolCenterLon;
    m_patrolLat.set_text(latText.str());
    m_patrolLon.set_text(lonText.str());
  }

  void
  undoMissionWaypoint()
  {
    if (m_planWaypoints.empty()) {
      return;
    }
    m_planWaypoints.pop_back();
    updatePatrolInputsFromWaypoints();
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Removed last waypoint. Current mission waypoint count: " +
                          std::to_string(m_planWaypoints.size()) + ".");
    refreshMapTile();
  }

  void
  clearMissionWaypoints()
  {
    m_planWaypoints.clear();
    updatePatrolInputsFromWaypoints();
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Mission waypoints cleared. Click the map to append WP1/WP2/...");
    refreshMapTile();
  }

  void
  scheduleMissionStartPhase(int phase, size_t droneIndex)
  {
    const auto startableDrones = m_runtime.missionStartableDrones();
    if (startableDrones.empty()) {
      m_status.set_text("No uploaded patrol mission is ready; upload mission before Start Mission");
      return;
    }
    if (droneIndex >= startableDrones.size()) {
      if (phase == 0) {
        m_status.set_text("Mission sequence: all patrol drones armed; taking off next");
        Glib::signal_timeout().connect([this] {
          scheduleMissionStartPhase(1, 0);
          return false;
        }, 1800);
        return;
      }
      if (phase == 1) {
        m_status.set_text("Mission sequence: all patrol drones takeoff sent; starting missions next");
        Glib::signal_timeout().connect([this] {
          scheduleMissionStartPhase(2, 0);
          return false;
        }, 6500);
        return;
      }
      m_status.set_text("Mission start sequence sent to patrol drones");
      updateVehicleRows();
      return;
    }

    const auto droneId = startableDrones[droneIndex];
    if (phase == 0) {
      m_status.set_text("Mission sequence: arming Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(startableDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "arm", {{"arm", "true"}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(0, droneIndex + 1);
        return false;
      }, 900);
      return;
    }
    if (phase == 1) {
      m_status.set_text("Mission sequence: takeoff Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(startableDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(1, droneIndex + 1);
        return false;
      }, 900);
      return;
    }

    m_status.set_text("Mission sequence: starting mission on Drone " + droneId +
                      " (" + std::to_string(droneIndex + 1) + "/" +
                      std::to_string(startableDrones.size()) + ")");
    m_runtime.sendMavlinkCommandToDrone(droneId, "start_mission");
    Glib::signal_timeout().connect([this, droneIndex] {
      scheduleMissionStartPhase(2, droneIndex + 1);
      return false;
    }, 900);
  }

  void
  schedulePatrolLandSequence(size_t droneIndex)
  {
    if (droneIndex >= m_droneIds.size()) {
      m_status.set_text("Stop Patrol sequence sent to patrol drones");
      updateVehicleRows();
      return;
    }
    const auto droneId = m_droneIds[droneIndex];
    m_status.set_text("Stop Patrol: landing Drone " + droneId);
    m_runtime.sendMavlinkCommandToDrone(droneId, "land");
    Glib::signal_timeout().connect([this, droneIndex] {
      schedulePatrolLandSequence(droneIndex + 1);
      return false;
    }, 1800);
  }

  std::pair<double, double>
  mapEventToImagePixel(double pixelX, double pixelY)
  {
    const auto allocation = m_mapImage.get_allocation();
    const auto width = std::max(1, allocation.get_width());
    const auto height = std::max(1, allocation.get_height());
    const auto imageX = std::clamp(pixelX - static_cast<double>(allocation.get_x()),
                                   0.0, static_cast<double>(width - 1));
    const auto imageY = std::clamp(pixelY - static_cast<double>(allocation.get_y()),
                                   0.0, static_cast<double>(height - 1));
    return {
      imageX * 256.0 / static_cast<double>(width),
      imageY * 256.0 / static_cast<double>(height)
    };
  }

  void
  beginMapDrag(double pixelX, double pixelY)
  {
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    m_mapDragging = true;
    m_mapDragStartX = imagePixel.first;
    m_mapDragStartY = imagePixel.second;
    const auto [tileX, tileY] = tileFloatForLatLon(m_mapCenterLat, m_mapCenterLon, m_mapZoom);
    m_mapDragStartTileX = tileX;
    m_mapDragStartTileY = tileY;
  }

  void
  finishMapDrag(double pixelX, double pixelY)
  {
    if (!m_mapDragging) {
      return;
    }
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    m_mapDragging = false;
    const auto dx = imagePixel.first - m_mapDragStartX;
    const auto dy = imagePixel.second - m_mapDragStartY;
    if (dx * dx + dy * dy < 36.0) {
      placePatrolCenterFromMapClick(imagePixel.first, imagePixel.second);
      return;
    }
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Map panned. Click to append a waypoint, or press Center GS.");
    refreshMapTile();
  }

  void
  updateMapDrag(double pixelX, double pixelY)
  {
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    const auto dx = imagePixel.first - m_mapDragStartX;
    const auto dy = imagePixel.second - m_mapDragStartY;
    if (dx * dx + dy * dy < 16.0) {
      return;
    }
    const auto newTileX = m_mapDragStartTileX - dx / 256.0;
    const auto newTileY = m_mapDragStartTileY - dy / 256.0;
    const auto [lat, lon] = latLonForTileFloat(newTileX, newTileY, m_mapZoom);
    m_mapCenterLat = lat;
    m_mapCenterLon = lon;
    refreshMapTile();
  }

  void
  zoomMap(int delta)
  {
    m_mapZoom = std::clamp(m_mapZoom + delta, 2, 19);
    refreshMapTile();
  }

  void
  centerMapOnGroundStation()
  {
    m_mapCenterLat = m_groundStationLat;
    m_mapCenterLon = m_groundStationLon;
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Map centered on GS / University of Memphis.\n"
                          "Click to append mission waypoints, drag to pan, +/- to zoom.");
    refreshMapTile();
  }

  void
  placePatrolCenterFromMapClick(double pixelX, double pixelY)
  {
    const auto latLon = latLonForTileFloat(
      (m_mapSourcePixelX + std::clamp(pixelX, 0.0, 255.0)) / 256.0,
      (m_mapSourcePixelY + std::clamp(pixelY, 0.0, 255.0)) / 256.0,
      m_mapZoom);
    m_planWaypoints.push_back(latLon);
    updatePatrolInputsFromWaypoints();
    std::ostringstream pointText;
    pointText << std::fixed << std::setprecision(6)
              << latLon.first << "," << latLon.second;
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "GS center: University of Memphis\n"
                          "Added WP" + std::to_string(m_planWaypoints.size()) +
                          " at " + pointText.str() + "\n"
                          "Upload Patrol Mission sends this route; Undo/Clear edits it.");
    refreshMapTile();
  }

  void
  refreshMapTile()
  {
    const int zoom = m_mapZoom;
    const auto [xFloat, yFloat] = tileFloatForLatLon(m_mapCenterLat, m_mapCenterLon, zoom);
    const auto centerPixelX = xFloat * 256.0;
    const auto centerPixelY = yFloat * 256.0;
    const auto sourcePixelX = centerPixelX - 128.0;
    const auto sourcePixelY = centerPixelY - 128.0;
    const auto baseTileX = static_cast<int>(std::floor(sourcePixelX / 256.0));
    const auto baseTileY = static_cast<int>(std::floor(sourcePixelY / 256.0));
    const auto cropX = std::clamp(static_cast<int>(std::round(sourcePixelX - baseTileX * 256.0)), 0, 255);
    const auto cropY = std::clamp(static_cast<int>(std::round(sourcePixelY - baseTileY * 256.0)), 0, 255);
    m_mapTileX = baseTileX;
    m_mapTileY = baseTileY;
    m_mapSourcePixelX = sourcePixelX;
    m_mapSourcePixelY = sourcePixelY;
    const auto generation = ++m_mapRenderGeneration;
    const auto markers = currentMapMarkers();
    const auto tileKey = std::to_string(zoom) + "/" + std::to_string(baseTileX) +
                         "/" + std::to_string(baseTileY) + "/" +
                         std::to_string(cropX / 8) + "/" + std::to_string(cropY / 8);
    if (m_mapTileLoading.load()) {
      m_mapRefreshPending = true;
      return;
    }
    m_mapRefreshPending = false;
    m_mapTileKey = tileKey;
    m_mapTileLoading = true;
    std::thread([this, zoom, baseTileX, baseTileY, cropX, cropY,
                 sourcePixelX, sourcePixelY, markers, generation] {
      struct LoadedTile
      {
        int x = 0;
        int y = 0;
        std::string path;
      };

      std::vector<LoadedTile> tiles;
      bool ok = true;
      for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
          const auto tileX = baseTileX + dx;
          const auto tileY = baseTileY + dy;
          const auto tileKey = std::to_string(zoom) + "/" + std::to_string(tileX) +
                               "/" + std::to_string(tileY);
          auto tilePath = "NDNSF-UAV-APP/maps/osm/" + std::to_string(zoom) +
                          "/" + std::to_string(tileX) + "/" +
                          std::to_string(tileY) + ".png";
          auto tileOk = access(tilePath.c_str(), R_OK) == 0;
          if (!tileOk) {
            tilePath = "/tmp/ndnsf-uav-map-" + std::to_string(zoom) + "-" +
                       std::to_string(tileX) + "-" + std::to_string(tileY) + ".png";
            tileOk = access(tilePath.c_str(), R_OK) == 0;
          }
          if (!tileOk) {
            const auto url = "https://tile.openstreetmap.org/" + tileKey + ".png";
            const auto command = "curl -fsSL --max-time 5 -A ndnsf-uav-app -o " +
                                 shellQuote(tilePath) + " " + shellQuote(url);
            tileOk = std::system(command.c_str()) == 0;
          }
          ok = ok && tileOk;
          if (tileOk) {
            tiles.push_back({tileX, tileY, tilePath});
          }
        }
      }
      Glib::signal_idle().connect_once([this, tiles, ok, baseTileX, baseTileY,
                                        cropX, cropY, sourcePixelX, sourcePixelY,
                                        markers, zoom, generation] {
        m_mapTileLoading = false;
        auto refreshPending = [this] {
          if (m_mapRefreshPending.exchange(false)) {
            refreshMapTile();
          }
        };
        if (generation != m_mapRenderGeneration.load()) {
          refreshPending();
          return;
        }
        if (!ok) {
          refreshPending();
          return;
        }
        try {
          auto canvas = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 512, 512);
          canvas->fill(0xffffffff);
          for (const auto& tile : tiles) {
            auto pixbuf = Gdk::Pixbuf::create_from_file(tile.path, 256, 256, true);
            if (pixbuf) {
              pixbuf->copy_area(0, 0, 256, 256, canvas,
                                (tile.x - baseTileX) * 256,
                                (tile.y - baseTileY) * 256);
            }
          }
          auto marked = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 256, 256);
          canvas->copy_area(cropX, cropY, 256, 256, marked, 0, 0);
            for (const auto& marker : markers) {
              const auto [mxFloat, myFloat] = tileFloatForLatLon(marker.lat, marker.lon, zoom);
            const auto markerX = static_cast<int>(std::round(mxFloat * 256.0 - sourcePixelX));
            const auto markerY = static_cast<int>(std::round(myFloat * 256.0 - sourcePixelY));
            if (markerX < -24 || markerY < -24 || markerX > 280 || markerY > 280) {
              continue;
            }
              drawMapMarker(marked, markerX, markerY, marker.label, marker.r, marker.g, marker.b);
            }
            m_mapImage.set(marked);
        }
        catch (const Glib::Error& e) {
          NDN_LOG_WARN("GS_MAP_TILE_LOAD_FAILED " << e.what());
        }
        refreshPending();
      });
    }).detach();
  }

private:
  GroundStationServiceContainer& m_runtime;
  Gtk::Box m_box;
  Gtk::Box m_buttons;
  Gtk::Box m_patrolControls;
  Gtk::Box m_workspace;
  Gtk::Frame m_vehicleFrame;
  Gtk::Box m_vehiclePanel;
  Gtk::Label m_vehicleHint;
  Gtk::ListBox m_vehicleList;
  Gtk::Frame m_centerFrame;
  Gtk::Box m_centerPanel;
  Gtk::Box m_flyContent;
  Gtk::Frame m_mapFrame;
  Gtk::Box m_mapPanel;
  Gtk::Box m_mapControls;
  Gtk::EventBox m_mapEventBox;
  Gtk::Image m_mapImage;
  Gtk::Label m_mapMission;
  Gtk::Frame m_videoFrame;
  Gtk::Box m_videoPanel;
  Gtk::Frame m_statusFrame;
  Gtk::Box m_statusPanel;
  Gtk::Button m_start;
  Gtk::Button m_stop;
  Gtk::Button m_arm;
  Gtk::Button m_takeoff;
  Gtk::Button m_land;
  Gtk::Button m_emergencyStop;
  Gtk::Button m_patrol;
  Gtk::Button m_startMission;
  Gtk::Button m_stopPatrol;
  Gtk::Button m_controlToggle;
  Gtk::Button m_refreshRecording;
  Gtk::Button m_playRecording;
  Gtk::Button m_mapZoomIn;
  Gtk::Button m_mapZoomOut;
  Gtk::Button m_mapCenterGs;
  Gtk::Button m_mapUndoWp;
  Gtk::Button m_mapClearWp;
  Gtk::Label m_patrolHint;
  Gtk::Entry m_patrolLat;
  Gtk::Entry m_patrolLon;
  Gtk::Entry m_patrolSizeMeters;
  Gtk::Box m_controlPanel;
  Gtk::Box m_inputModeRow;
  Gtk::Label m_inputModeHint;
  Gtk::ToggleButton m_keyboardMode;
  Gtk::ToggleButton m_gamepadMode;
  Gtk::Box m_controlLayout;
  Gtk::Frame m_keyboardLeftFrame;
  Gtk::Frame m_keyboardRightFrame;
  Gtk::Frame m_keyboardCommandFrame;
  Gtk::Frame m_gamepadFrame;
  Gtk::Grid m_keyboardLeftGrid;
  Gtk::Grid m_keyboardRightGrid;
  Gtk::Grid m_keyboardCommandGrid;
  Gtk::Grid m_gamepadGrid;
  Gtk::Label m_keyboardLeftLabel;
  Gtk::Label m_keyboardRightLabel;
  Gtk::Label m_controlHelp;
  Gtk::Button m_keyW;
  Gtk::Button m_keyA;
  Gtk::Button m_keyS;
  Gtk::Button m_keyD;
  Gtk::Button m_keyQ;
  Gtk::Button m_keyE;
  Gtk::Button m_keyR;
  Gtk::Button m_keyF;
  Gtk::Button m_keyI;
  Gtk::Button m_keyT;
  Gtk::Button m_keyL;
  Gtk::Button m_keyV;
  Gtk::Button m_keyX;
  Gtk::Button m_padLeftStick;
  Gtk::Button m_padRightStick;
  Gtk::Button m_padA;
  Gtk::Button m_padB;
  Gtk::Button m_padX;
  Gtk::Button m_padY;
  Gtk::Button m_padLB;
  Gtk::Button m_padRB;
  Gtk::Label m_status;
  Gtk::Label m_linkStatus;
  Gtk::Label m_services;
  Gtk::Label m_telemetry;
  Gtk::Image m_image;
  Gtk::Label m_stats;
  Glib::Dispatcher m_statusDispatcher;
  Glib::Dispatcher m_frameDispatcher;
  Glib::Dispatcher m_gamepadDispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
  std::string m_pendingLinkStatus;
  std::string m_pendingTelemetry;
  std::string m_pendingMap;
  bool m_pendingMapRefresh = false;
  std::string m_pendingMapLat = "35.1186";
  std::string m_pendingMapLon = "-89.9375";
  bool m_pendingVehicleRowsRefresh = false;
  std::string m_mapTileKey;
  std::atomic<bool> m_mapTileLoading{false};
  std::atomic<bool> m_mapRefreshPending{false};
  std::atomic<uint64_t> m_mapRenderGeneration{0};
  const double m_groundStationLat = 35.1186;
  const double m_groundStationLon = -89.9375;
  double m_mapCenterLat = 35.1186;
  double m_mapCenterLon = -89.9375;
  int m_mapZoom = 15;
  int m_mapTileX = 0;
  int m_mapTileY = 0;
  double m_mapSourcePixelX = 0.0;
  double m_mapSourcePixelY = 0.0;
  bool m_mapDragging = false;
  double m_mapDragStartX = 0.0;
  double m_mapDragStartY = 0.0;
  double m_mapDragStartTileX = 0.0;
  double m_mapDragStartTileY = 0.0;
  bool m_hasPatrolCenter = false;
  double m_patrolCenterLat = 35.1186;
  double m_patrolCenterLon = -89.9375;
  std::vector<std::pair<double, double>> m_planWaypoints;
  std::map<std::string, std::pair<double, double>> m_dronePositions;
  size_t m_telemetryPollIndex = 0;
  Glib::RefPtr<Gdk::Pixbuf> m_pendingPixbuf;
  uint64_t m_pendingSeq = 0;
  uint64_t m_pendingElapsedMs = 0;
  bool m_pendingButtonState = false;
  bool m_pendingStartSensitive = true;
  bool m_pendingStopSensitive = false;
  bool m_pendingClearFrame = false;
  bool m_controlMode = false;
  bool m_updatingInputMode = false;
  bool m_gamepadAvailable = false;
  std::string m_gamepadDevicePath;
  std::set<guint> m_pressedKeys;
  std::thread m_manualControlThread;
  std::thread m_gamepadThread;
  std::atomic<bool> m_manualControlDone{false};
  std::atomic<bool> m_gamepadDone{false};
  std::atomic<bool> m_useGamepad{false};
  std::atomic<bool> m_manualActive{false};
  std::atomic<int> m_manualX{0};
  std::atomic<int> m_manualY{0};
  std::atomic<int> m_manualZ{500};
  std::atomic<int> m_manualR{0};
  std::array<std::atomic<int>, 8> m_gamepadAxes{};
  std::array<std::atomic<bool>, 16> m_gamepadButtons{};
  bool m_autoRepeatStopTest = false;
  uint64_t m_streamGeneration = 0;
  bool m_acceptFrames = false;
  std::atomic<uint64_t> m_decodedFrames{0};
  std::vector<std::string> m_droneIds;
};
