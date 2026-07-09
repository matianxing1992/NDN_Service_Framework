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
                      bool autoVideoSelectionTest,
                      bool autoMissionControlsTest,
                      bool autoFlightControlsTest,
                      bool autoRecordingPlaybackTest,
                      bool autoDashboardPanelTest,
                      bool autoApplyBitrateTest,
                      bool autoVideoPressureProfileTest,
                      bool autoRepeatStopTest,
                      std::vector<std::string> droneIds,
                      double groundStationMapLat,
                      double groundStationMapLon,
                      uint64_t operatorAuthorityRefreshIntervalMs)
    : m_runtime(runtime)
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
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
    , m_applyBitrate("Bitrate")
    , m_arm("Arm")
    , m_takeoff("Takeoff")
    , m_land("Land")
    , m_emergencyStop("E-Stop")
    , m_patrol("Upload Mission")
    , m_startMission("Start Mission")
    , m_stopPatrol("Stop Patrol")
    , m_saveMissionPlan("Save Plan")
    , m_loadMissionPlan("Load Plan")
    , m_controlToggle("Control")
    , m_refreshRecording("Find Rec.")
    , m_browseRepo("Browse Repo")
    , m_fetchParameters("Fetch Params")
    , m_refreshAuthority("Refresh Lease")
    , m_playRecording("Play Rec.")
    , m_mapZoomIn("+")
    , m_mapZoomOut("-")
    , m_mapPanMode("Pan")
    , m_mapFollowSelected("Follow Selected")
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
    , m_groundStationLat(std::isfinite(groundStationMapLat) ?
                        groundStationMapLat : DEFAULT_GS_MAP_LAT)
    , m_groundStationLon(std::isfinite(groundStationMapLon) ?
                        groundStationMapLon : DEFAULT_GS_MAP_LON)
    , m_mapCenterLat(std::isfinite(groundStationMapLat) ?
                    groundStationMapLat : DEFAULT_GS_MAP_LAT)
    , m_mapCenterLon(std::isfinite(groundStationMapLon) ?
                    groundStationMapLon : DEFAULT_GS_MAP_LON)
    , m_autoApplyBitrateTest(autoApplyBitrateTest)
    , m_autoVideoPressureProfileTest(autoVideoPressureProfileTest)
    , m_autoRepeatStopTest(autoRepeatStopTest)
    , m_autoDashboardPanelTest(autoDashboardPanelTest)
    , m_operatorAuthorityRefreshIntervalMs(operatorAuthorityRefreshIntervalMs)
    , m_droneIds(std::move(droneIds))
  {
    set_title("NDNSF UAV Ground Station");
    set_default_size(1600, 800);
    set_border_width(12);
    set_can_focus(true);
    if (m_droneIds.empty()) {
      m_droneIds.push_back("A");
    }

    m_status.set_text("Video stopped");
    m_stats.set_text("Frames: 0");
    m_linkStatus.set_text("Link RTT: waiting");
    m_mapMission.set_text(mapWorkspaceStatusText("ready", m_runtime.targetDroneId(),
                                                 "click-map-to-add-waypoint"));
    m_services.set_text(m_runtime.serviceCatalogForDrone(m_runtime.targetDroneId()));
    m_telemetry.set_text("Telemetry: waiting for flight-controller response");
    m_stop.set_sensitive(false);
    m_startMission.set_sensitive(false);
    m_stopPatrol.set_sensitive(false);

    m_buttons.set_selection_mode(Gtk::SELECTION_NONE);
    m_buttons.set_row_spacing(6);
    m_buttons.set_column_spacing(6);
    m_buttons.set_homogeneous(false);
    m_buttons.set_min_children_per_line(2);
    m_buttons.set_max_children_per_line(7);
    m_buttons.set_hexpand(true);
    for (auto* button : {&m_start, &m_stop, &m_applyBitrate, &m_arm, &m_takeoff,
                         &m_land, &m_emergencyStop, &m_patrol, &m_startMission,
                         &m_stopPatrol, &m_controlToggle, &m_refreshRecording,
                         &m_browseRepo, &m_fetchParameters, &m_refreshAuthority,
                         &m_playRecording}) {
      button->set_size_request(104, -1);
      button->set_hexpand(false);
      button->set_halign(Gtk::ALIGN_START);
      m_buttons.insert(*button, -1);
    }
    m_box.pack_start(m_buttons, Gtk::PACK_SHRINK);

    m_patrolHint.set_text("Patrol center / size");
    {
      std::ostringstream latText;
      std::ostringstream lonText;
      latText << std::fixed << std::setprecision(6) << m_groundStationLat;
      lonText << std::fixed << std::setprecision(6) << m_groundStationLon;
      m_patrolLat.set_text(latText.str());
      m_patrolLon.set_text(lonText.str());
    }
    m_patrolSizeMeters.set_text("140");
    m_missionPlanFile.set_text(m_runtime.missionPlanFilePath());
    m_patrolLat.set_width_chars(9);
    m_patrolLon.set_width_chars(10);
    m_patrolSizeMeters.set_width_chars(6);
    m_missionPlanFile.set_width_chars(30);
    m_missionPlanFile.set_tooltip_text("Mission plan save/load file path");
    m_saveMissionPlan.set_tooltip_text("Save the current mission preview or uploaded plan");
    m_loadMissionPlan.set_tooltip_text("Load a saved mission plan file into the ground-station preview");
    m_patrolControls.pack_start(m_patrolHint, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLat, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLon, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolSizeMeters, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_missionPlanFile, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_saveMissionPlan, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_loadMissionPlan, Gtk::PACK_SHRINK);
    m_box.pack_start(m_patrolControls, Gtk::PACK_SHRINK);

    m_workspace.set_size_request(-1, 600);
    m_workspace.set_vexpand(false);

    m_vehiclePanel.set_border_width(8);
    m_vehicleHint.set_text("Connected / expected drones");
    m_vehicleHint.set_size_request(236, -1);
    m_vehicleHint.set_line_wrap(true);
    m_vehicleHint.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    m_vehiclePanel.pack_start(m_vehicleHint, Gtk::PACK_SHRINK);
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      const auto selected = i == 0;
      auto* rowLabel = Gtk::manage(new Gtk::Label(
        std::string(selected ? "● " : "○ ") + "Drone " + m_droneIds[i] +
        (selected ? "  active" : "  standby")));
      rowLabel->set_xalign(0.0F);
      rowLabel->set_size_request(236, -1);
      rowLabel->set_width_chars(28);
      rowLabel->set_max_width_chars(28);
      rowLabel->set_line_wrap(true);
      rowLabel->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
      rowLabel->set_ellipsize(Pango::ELLIPSIZE_NONE);
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
      applyFollowSelectedToMap(m_droneIds[static_cast<size_t>(index)]);
      m_pendingMapRefresh = true;
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
    m_mapMission.set_size_request(300, -1);
    m_mapMission.set_width_chars(36);
    m_mapMission.set_max_width_chars(36);
    m_mapMission.set_line_wrap(true);
    m_mapMission.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    m_mapZoomIn.set_tooltip_text("Zoom in");
    m_mapZoomOut.set_tooltip_text("Zoom out");
    m_mapPanMode.set_tooltip_text("Enable/disable left-drag map panning");
    m_mapFollowSelected.set_tooltip_text("Keep map centered on selected drone");
    m_mapCenterGs.set_tooltip_text("Return map view to the ground station");
    m_mapUndoWp.set_tooltip_text("Remove the last mission waypoint");
    m_mapClearWp.set_tooltip_text("Clear all mission waypoints");
    m_mapControls.pack_start(m_mapZoomIn, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapZoomOut, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapPanMode, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapFollowSelected, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapCenterGs, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapUndoWp, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapClearWp, Gtk::PACK_SHRINK);
    m_mapPanel.set_border_width(6);
    m_mapFrame.set_size_request(320, 580);
    m_mapFrame.set_hexpand(false);
    m_mapPanel.set_size_request(312, -1);
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
      if (m_mapPanMode.get_active()) {
        beginMapDrag(event->x, event->y);
      } else {
        beginMapPointer(event->x, event->y);
      }
      return true;
    });
    m_mapEventBox.signal_button_release_event().connect([this](GdkEventButton* event) {
      if (event == nullptr || event->button != 1) {
        return false;
      }
      if (m_mapPanMode.get_active()) {
        finishMapDrag(event->x, event->y);
      } else {
        finishMapPointer(event->x, event->y);
      }
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
      const bool shouldZoomIn = (event->direction == GDK_SCROLL_UP ||
                               (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0.0));
      const bool shouldZoomOut = (event->direction == GDK_SCROLL_DOWN ||
                                (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0.0));
      if (!shouldZoomIn && !shouldZoomOut) {
        return false;
      }
      const auto imagePixel = mapEventToImagePixel(event->x, event->y);
      const int zoomDelta = shouldZoomIn ? 1 : -1;
      zoomMapAt(zoomDelta, imagePixel.first, imagePixel.second);
      return true;
    });
    m_mapPanel.pack_start(m_mapEventBox, Gtk::PACK_SHRINK);
    m_mapPanel.pack_start(m_mapMission, Gtk::PACK_SHRINK);
    m_mapFrame.add(m_mapPanel);
    m_flyContent.pack_start(m_mapFrame, Gtk::PACK_SHRINK);
    m_videoPanel.set_border_width(6);
    m_videoFrame.set_size_request(560, 360);
    m_videoFrame.set_hexpand(false);
    m_videoFrame.set_vexpand(false);
    m_image.set_size_request(520, 300);
    m_image.set_halign(Gtk::ALIGN_CENTER);
    m_image.set_valign(Gtk::ALIGN_CENTER);
    m_videoPanel.pack_start(m_image, Gtk::PACK_SHRINK);
    m_stats.set_size_request(520, -1);
    m_stats.set_width_chars(60);
    m_stats.set_max_width_chars(60);
    m_stats.set_line_wrap(true);
    m_stats.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    m_videoPanel.pack_start(m_stats, Gtk::PACK_SHRINK);
    m_videoFrame.add(m_videoPanel);
    m_flyContent.pack_start(m_videoFrame, Gtk::PACK_SHRINK);
    m_centerPanel.pack_start(m_flyContent, Gtk::PACK_SHRINK);
    m_centerFrame.add(m_centerPanel);
    m_centerFrame.set_size_request(920, 600);
    m_centerFrame.set_hexpand(false);
    m_workspace.pack_start(m_centerFrame, Gtk::PACK_SHRINK);

    m_statusPanel.set_border_width(8);
    m_vehicleFrame.set_size_request(260, -1);
    m_vehicleFrame.set_hexpand(false);
    m_vehicleList.set_size_request(240, -1);
    m_statusFrame.set_size_request(300, -1);
    m_statusFrame.set_hexpand(false);
    m_status.set_xalign(0.0F);
    m_stats.set_xalign(0.0F);
    m_services.set_xalign(0.0F);
    m_telemetry.set_xalign(0.0F);
    m_linkStatus.set_xalign(0.0F);
    for (auto* label : {&m_status, &m_linkStatus, &m_services, &m_telemetry,
                         &m_dashboardInspector,
                         &m_flightInspector, &m_cameraInspector, &m_videoInspector,
                         &m_commandHistory, &m_telemetryInspector, &m_missionInspector,
                         &m_missionDetail, &m_authorityInspector,
                         &m_catalogInspector, &m_parameterInspector,
                         &m_functionalityInspector,
                         &m_practicalityInspector, &m_stabilityInspector}) {
      label->set_size_request(264, -1);
      label->set_width_chars(32);
      label->set_max_width_chars(32);
      label->set_line_wrap(true);
      label->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
      label->set_ellipsize(Pango::ELLIPSIZE_NONE);
      label->set_selectable(true);
    }
    auto addInspectorSection = [this](Gtk::Frame& frame, Gtk::Label& label,
                                      const std::string& title) {
      frame.set_label(title);
      frame.set_shadow_type(Gtk::SHADOW_ETCHED_IN);
      frame.add(label);
      m_statusPanel.pack_start(frame, Gtk::PACK_SHRINK);
    };
    addInspectorSection(m_dashboardInspectorFrame, m_dashboardInspector, "Vehicle Summary");
    addInspectorSection(m_flightInspectorFrame, m_flightInspector, "Flight Controller");
    addInspectorSection(m_telemetryInspectorFrame, m_telemetryInspector, "Telemetry");
    addInspectorSection(m_cameraInspectorFrame, m_cameraInspector, "Camera");
    addInspectorSection(m_videoInspectorFrame, m_videoInspector, "Video");
    addInspectorSection(m_missionInspectorFrame, m_missionInspector, "Mission / Safety");
    addInspectorSection(m_missionDetailFrame, m_missionDetail, "Mission Detail");
    addInspectorSection(m_authorityInspectorFrame, m_authorityInspector, "Operator Authority");
    addInspectorSection(m_catalogInspectorFrame, m_catalogInspector, "Repo Catalog");
    addInspectorSection(m_parameterInspectorFrame, m_parameterInspector, "Vehicle Parameters");
    addInspectorSection(m_functionalityInspectorFrame, m_functionalityInspector, "Functionality");
    addInspectorSection(m_practicalityInspectorFrame, m_practicalityInspector, "Practicality");
    addInspectorSection(m_stabilityInspectorFrame, m_stabilityInspector, "Stability");
    addInspectorSection(m_commandHistoryFrame, m_commandHistory, "Command History");
    addInspectorSection(m_serviceInspectorFrame, m_services, "Services / Link");
    addInspectorSection(m_eventInspectorFrame, m_status, "Events");
    m_statusScroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_statusScroll.set_size_request(286, 540);
    m_statusScroll.set_min_content_height(540);
    m_statusScroll.add(m_statusPanel);
    m_statusFrame.add(m_statusScroll);
    m_workspace.pack_start(m_statusFrame, Gtk::PACK_SHRINK);
    updateInspectorPanel();

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
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.set_min_content_height(760);
    m_scroll.set_hexpand(true);
    m_scroll.set_vexpand(true);
    m_scroll.add(m_box);
    add(m_scroll);
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
    m_applyBitrate.signal_clicked().connect([this] {
      m_runtime.applySuggestedVideoBitrate();
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
      m_patrolUploadInFlight = true;
      updateSelectedActionControls();
      m_status.set_text("Uploading mission plan...");
      const auto editablePlan = currentEditableMissionPlan();
      if (editablePlan) {
        std::thread([this, editablePlan] {
          const bool ok = m_runtime.uploadMissionPlan(*editablePlan, std::chrono::seconds(30));
          Glib::signal_idle().connect_once([this, ok] {
            m_patrolUploadInFlight = false;
            m_status.set_text(ok ? "Mission plan uploaded; arm/takeoff and start mission mode to fly it"
                                 : "Mission plan upload failed");
            updateVehicleRows();
          });
        }).detach();
        return;
      }
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
          m_patrolUploadInFlight = false;
          m_status.set_text(ok ? "Patrol mission uploaded; arm/takeoff and start mission mode to fly it"
                               : "Patrol mission upload failed");
          updateVehicleRows();
        });
      }).detach();
    });
    m_startMission.signal_clicked().connect([this] {
      m_missionStartInFlight = true;
      m_status.set_text("Starting patrol mission by phase: arm all, take off all, start all...");
      updateSelectedActionControls();
      scheduleMissionStartPhase(0, 0);
    });
    m_stopPatrol.signal_clicked().connect([this] {
      if (m_patrolStopInFlight.load()) {
        m_status.set_text("Stopping patrol in progress...");
        return;
      }

      const auto activePatrolMission = m_runtime.activePatrolMissionId();
      if (!activePatrolMission.has_value()) {
        m_status.set_text("No active patrol mission to stop");
        return;
      }

      m_patrolStopInFlight = true;
      const auto missionId = *activePatrolMission;
      if (m_runtime.cancelCurrentPatrolMission()) {
        m_status.set_text("Cancelling patrol mission: " + missionId);
      }
      else {
        m_status.set_text("Patrol stop already requested for mission: " + missionId);
      }
      updateSelectedActionControls();
      schedulePatrolLandSequence(0);
    });
    m_controlToggle.signal_clicked().connect([this] {
      setControlMode(!m_controlMode);
      updateFlightControlControls();
    });
    m_refreshRecording.signal_clicked().connect([this] {
      m_runtime.requestRecordingManifest();
    });
    m_browseRepo.signal_clicked().connect([this] {
      m_runtime.requestRepoCatalog();
    });
    m_fetchParameters.signal_clicked().connect([this] {
      m_runtime.requestVehicleParameters();
    });
    m_refreshAuthority.signal_clicked().connect([this] {
      triggerAuthorityLeaseRefresh("button");
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
      zoomMapAt(1, 128.0, 128.0);
    });
    m_mapZoomOut.signal_clicked().connect([this] {
      zoomMapAt(-1, 128.0, 128.0);
    });
    m_mapCenterGs.signal_clicked().connect([this] {
      centerMapOnGroundStation();
    });
    m_mapFollowSelected.signal_toggled().connect([this] {
      const auto selected = m_runtime.targetDroneId();
      applyFollowSelectedToMap(selected);
      m_pendingMapRefresh = true;
      m_statusDispatcher.emit();
    });
    m_mapUndoWp.signal_clicked().connect([this] {
      undoMissionWaypoint();
    });
    m_mapClearWp.signal_clicked().connect([this] {
      clearMissionWaypoints();
    });
    m_saveMissionPlan.signal_clicked().connect([this] {
      saveMissionPlanFile();
    });
    m_loadMissionPlan.signal_clicked().connect([this] {
      loadMissionPlanFile();
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
                 status.rfind("Video stop timed out", 0) == 0 ||
                 status.rfind("Video control response missing", 0) == 0 ||
                 status.rfind("NDNSF runtime not ready", 0) == 0 ||
                 status.rfind("No video streaming for selected drone ", 0) == 0 ||
                 status.rfind("Video already streaming drone=", 0) == 0) {
          updateVideoViewForSelectedLocked();
          const auto view = selectedDroneViewState();
          if (view.hasTelemetry) {
            m_pendingMap = view.mapText;
          }
          m_pendingVehicleRowsRefresh = true;
        }
        if (status.rfind("MAVLink ", 0) == 0 ||
            status.rfind("Telemetry ", 0) == 0 ||
            status.rfind("VideoAdaptive ", 0) == 0) {
          const auto statusDrone = statusField(status, "drone", m_runtime.targetDroneId());
          const auto selectedDrone = m_runtime.targetDroneId();
          if (status.rfind("Telemetry ", 0) == 0) {
            const auto telemetry = m_runtime.telemetryForDrone(statusDrone);
            if (telemetry) {
              try {
                m_dronePositions[statusDrone] = {
                  std::stod(telemetry->lat), std::stod(telemetry->lon)
                };
                maybeFollowSelectedDrone(statusDrone);
                m_pendingMapRefresh = true;
              }
              catch (const std::exception&) {
              }
              if (statusDrone == selectedDrone) {
                const auto view = selectedDroneViewState();
                m_pendingTelemetry = view.inspectorText;
                m_pendingMap = view.mapText;
              }
              m_pendingVehicleRowsRefresh = true;
            }
            else if (statusDrone == selectedDrone) {
              m_pendingTelemetry = status;
            }
          }
          else if (statusDrone == selectedDrone) {
            const auto command = m_runtime.commandForDrone(statusDrone);
            const auto view = selectedDroneViewState();
            m_pendingTelemetry = view.hasTelemetry ? view.inspectorText :
                                 command ? command->statusLine() : view.inspectorText;
            if (view.hasTelemetry) {
              m_pendingMap = view.mapText;
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
    m_runtime.setFrameCallback([this](std::vector<uint8_t> frame, uint64_t seq,
                                      uint64_t elapsedMs, std::string streamId,
                                      uint64_t streamSessionEpoch) {
      pushEncodedChunk(std::move(frame), seq, elapsedMs, std::move(streamId), streamSessionEpoch);
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
        m_lastDisplayedSeq = 0;
        m_pendingClearFrame = false;
      }
      m_status.set_text(m_pendingStatus);
      if (!m_pendingTelemetry.empty()) {
        m_lastInspectorDetail = m_pendingTelemetry;
      }
      if (m_pendingVehicleRowsRefresh) {
        updateVehicleRows();
        m_pendingVehicleRowsRefresh = false;
      }
      updateInspectorPanel();
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
      uint64_t frameGeneration = 0;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        pixbuf = m_pendingPixbuf;
        seq = m_pendingSeq;
        elapsedMs = m_pendingElapsedMs;
        frameGeneration = m_pendingFrameGeneration;
      }
      if (pixbuf) {
        {
          std::lock_guard<std::mutex> guard(m_mutex);
          if (frameGeneration < m_lastDisplayedGeneration) {
            return;
          }
          if (frameGeneration > m_lastDisplayedGeneration) {
            m_lastDisplayedGeneration = frameGeneration;
            m_lastDisplayedSeq = 0;
          }
          if (m_lastDisplayedSeq != 0 && seq <= m_lastDisplayedSeq) {
            return;
          }
          m_lastDisplayedSeq = seq;
        }
        m_image.set(pixbuf);
        auto stats = "Decoded frames: " + std::to_string(m_decodedFrames.load()) +
                     "  latest chunk: " + std::to_string(seq) +
                     "  stream elapsed: " + std::to_string(elapsedMs) + " ms";
        const auto adaptive = m_runtime.videoAdaptiveForDrone(m_runtime.targetDroneId());
        if (adaptive) {
          stats += "  " + adaptive->streamHealthSummary() +
                   "  " + adaptive->compactSummary();
        }
        m_stats.set_text(stats);
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
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Glib::signal_idle().connect_once([this] {
          logVideoAdaptiveViewState("auto-video-active");
        });
        if (m_autoVideoPressureProfileTest) {
          m_runtime.injectVideoAdaptivePressureForTest("timeout", 95, 0, 0, 0, 400, 80, 0);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          Glib::signal_idle().connect_once([this] {
            logVideoAdaptiveViewState("auto-video-pressure-timeout");
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          m_runtime.injectVideoAdaptivePressureForTest("backlog", 0, 0, 0, 120, 1000, 0, 0);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          Glib::signal_idle().connect_once([this] {
            logVideoAdaptiveViewState("auto-video-pressure-backlog");
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          m_runtime.injectVideoAdaptivePressureForTest("probe", 0, 95, 0, 0, 1000, 0, 0);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          Glib::signal_idle().connect_once([this] {
            logVideoAdaptiveViewState("auto-video-pressure-probe");
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          m_runtime.injectVideoAdaptivePressureForTest("frame-gap", 0, 0, 0, 0, 1200, 0, 0);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          Glib::signal_idle().connect_once([this] {
            logVideoAdaptiveViewState("auto-video-pressure-frame-gap");
          });
        }
        if (m_autoApplyBitrateTest) {
          bool applied = false;
          for (int i = 0; i < 30 && !applied; ++i) {
            applied = m_runtime.applySuggestedVideoBitrate();
            if (!applied) {
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
          }
          NDN_LOG_INFO("AUTO_VIDEO_APPLY_BITRATE_ATTEMPT applied="
                       << (applied ? "true" : "false"));
          if (applied) {
            for (int i = 0; i < 80 && !m_runtime.isStreaming(); ++i) {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            Glib::signal_idle().connect_once([this] {
              logVideoAdaptiveViewState("auto-video-after-bitrate-apply");
            });
          }
        }
        std::this_thread::sleep_for(std::chrono::seconds(autoStopSeconds));
        m_runtime.stopVideo();
        if (m_autoRepeatStopTest) {
          std::this_thread::sleep_for(std::chrono::milliseconds(3500));
          Glib::signal_idle().connect_once([this] {
            logVideoControlState("auto-video-stop-timeout");
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          m_runtime.stopVideo();
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        Glib::signal_idle().connect_once([this] {
          logVideoAdaptiveViewState("auto-video-stopped");
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
        m_runtime.sendMavlinkCommand("arm");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.requestTelemetryStatusForDroneSync(m_runtime.targetDroneId(),
                                                     std::chrono::milliseconds(2500));
        m_runtime.sendMavlinkCommand("takeoff");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.requestTelemetryStatusForDroneSync(m_runtime.targetDroneId(),
                                                     std::chrono::milliseconds(2500));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_w);
          handleShortcutKeyPress(GDK_KEY_r);
        });
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logManualSafetyProbe("fresh");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyRelease(GDK_KEY_w);
          handleShortcutKeyRelease(GDK_KEY_r);
          setControlMode(false);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        logManualSafetyProbe("neutral-timeout");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        m_runtime.sendMavlinkCommand("land");
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
    if (autoVideoSelectionTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (m_droneIds.size() < 2) {
          Glib::signal_idle().connect_once([this] {
            m_status.set_text("Video selection test needs at least two drones");
            hide();
          });
          return;
        }
        const auto first = m_droneIds[0];
        const auto second = m_droneIds[1];
        Glib::signal_idle().connect_once([this, first] {
          m_runtime.setTargetDroneId(first);
          updateVehicleRows();
          updateVideoViewForSelected();
          logVideoControlState("initial-first");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_runtime.startVideo();
        for (int i = 0; i < 120 && !m_runtime.isStreamingForDrone(first); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Glib::signal_idle().connect_once([this, second] {
          m_runtime.setTargetDroneId(second);
          updateVehicleRows();
          updateVideoViewForSelected();
          logVideoControlState("second-after-first-streaming");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        Glib::signal_idle().connect_once([this, first] {
          m_runtime.setTargetDroneId(first);
          updateVehicleRows();
          updateVideoViewForSelected();
          logVideoControlState("first-streaming");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        m_runtime.stopVideo();
        for (int i = 0; i < 120; ++i) {
          const auto video = m_runtime.videoForDrone(first);
          if (!m_runtime.isStreamingForDrone(first) &&
              video && !video->isStreaming()) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Glib::signal_idle().connect_once([this] {
          logVideoControlState("after-stop");
          hide();
        });
      }).detach();
    }
    if (autoMissionControlsTest) {
      std::thread([this] {
        auto makeReadiness = [] (std::string droneId, bool ready) {
          ReadinessState readiness;
          readiness.droneId = std::move(droneId);
          readiness.heartbeatSeen = ready ? "true" : "false";
          readiness.flightControllerReady = ready ? "true" : "false";
          readiness.gpsReady = ready ? "true" : "false";
          readiness.ekfReady = ready ? "true" : "false";
          readiness.batteryReady = ready ? "true" : "false";
          readiness.armed = "false";
          readiness.mode = "STANDBY";
          readiness.landedStateName = "on-ground";
          readiness.readiness = ready ? "ready" : "not-ready";
          readiness.readinessReason = ready ? "ok" : "waiting-heartbeat";
          readiness.timestampMs = nowMilliseconds();
          return readiness;
        };

        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          m_planWaypoints = {
            {35.118600, -89.937500},
            {35.118950, -89.937200},
            {35.119250, -89.936850},
          };
          updatePatrolInputsFromWaypoints();
          refreshMissionPlanPreview("auto-mission-controls");
          updateVehicleRows();
          logMissionControlState("initial");
          logSelectedDroneViewState("preview");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          m_patrolUploadInFlight = true;
          updateVehicleRows();
          logMissionControlState("uploading");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this, makeReadiness] {
          for (const auto& droneId : m_droneIds) {
            m_runtime.injectReadinessStateForTest(makeReadiness(droneId, false));
            MissionState mission;
            mission.droneId = droneId;
            mission.missionId = "mission-controls-test";
            mission.partId = "part-" + droneId;
            mission.phase = "uploaded";
            mission.detail = "mission-controls-test";
            mission.ack = "test";
            mission.transport = "test";
            mission.waypointsForwarded = "4";
            mission.waypointAcksAccepted = "4";
            mission.updatedMs = nowMilliseconds();
            m_runtime.injectMissionStateForTest(std::move(mission));
          }
          m_patrolUploadInFlight = false;
          m_status.set_text("Mission controls test upload complete");
          updateVehicleRows();
          logMissionControlState("after-upload");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this, makeReadiness] {
          for (const auto& droneId : m_droneIds) {
            m_runtime.injectReadinessStateForTest(makeReadiness(droneId, true));
          }
          updateVehicleRows();
          logMissionControlState("after-ready");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          m_missionStartInFlight = true;
          updateVehicleRows();
          logMissionControlState("start-pending");
          m_missionStartInFlight = false;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          m_patrolStopInFlight = true;
          updateVehicleRows();
          logMissionControlState("stop-pending");
          m_patrolStopInFlight = false;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          MissionProgressState progress;
          progress.taskId = "mission-controls-progress-test";
          progress.phase = "compensating";
          progress.assignment = "clustered-waypoints-return-to-start";
          progress.drones = "A,B";
          progress.attempts = 2;
          progress.totalParts = 2;
          progress.completedParts = 1;
          progress.missingParts = 1;
          progress.returnHomePlanned = true;
          progress.completedPartIds = "part1";
          progress.missingPartIds = "part0";
          progress.pendingPartIds = "none";
          m_runtime.injectMissionProgressForTest(std::move(progress));
          updateVehicleRows();
          logMissionControlState("progress-active");
          logSelectedDroneViewState("progress-active");
          logDroneListRowState("progress-active");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          MissionProgressState progress;
          progress.taskId = "mission-controls-progress-test";
          progress.phase = "completed";
          progress.assignment = "clustered-waypoints-return-to-start";
          progress.drones = "A,B";
          progress.attempts = 2;
          progress.totalParts = 2;
          progress.completedParts = 2;
          progress.missingParts = 0;
          progress.compensatedParts = 1;
          progress.returnHomePlanned = true;
          progress.completedPartIds = "part0,part1";
          progress.missingPartIds = "none";
          progress.compensatedPartIds = "part0";
          progress.pendingPartIds = "none";
          m_runtime.injectMissionProgressForTest(std::move(progress));
          updateVehicleRows();
          logMissionControlState("progress-completed");
          logSelectedDroneViewState("progress-completed");
          logDroneListRowState("progress-completed");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          logMissionControlState("final");
          logAppQualityState("mission-controls-final");
          hide();
        });
      }).detach();
    }
    if (autoFlightControlsTest) {
      std::thread([this] {
        auto makeReadiness = [this] (std::string droneId, bool ready, bool armed) {
          ReadinessState readiness;
          readiness.droneId = std::move(droneId);
          readiness.heartbeatSeen = ready ? "true" : "false";
          readiness.flightControllerReady = ready ? "true" : "false";
          readiness.gpsReady = ready ? "true" : "false";
          readiness.ekfReady = ready ? "true" : "false";
          readiness.batteryReady = ready ? "true" : "false";
          readiness.armed = armed ? "true" : "false";
          readiness.mode = armed ? "GUIDED" : "STANDBY";
          readiness.landedStateName = armed ? "on-ground" : "unknown";
          readiness.readiness = ready ? "ready" : "not-ready";
          readiness.readinessReason = ready ? "ok" : "waiting-heartbeat";
          readiness.timestampMs = nowMilliseconds();
          return readiness;
        };

        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this, readiness = makeReadiness(m_runtime.targetDroneId(), false, false)] {
          m_runtime.injectReadinessStateForTest(readiness);
          updateVehicleRows();
          logFlightActionControlState("not-ready");
          logSelectedActionState("not-ready");
          logSelectedDroneViewState("not-ready");
          logDroneListRowState("not-ready");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this, readiness = makeReadiness(m_runtime.targetDroneId(), true, false)] {
          m_runtime.injectReadinessStateForTest(readiness);
          updateVehicleRows();
          logFlightActionControlState("ready-unarmed");
          logSelectedActionState("ready-unarmed");
          logSelectedDroneViewState("ready-unarmed");
          logDroneListRowState("ready-unarmed");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this, readiness = makeReadiness(m_runtime.targetDroneId(), true, true)] {
          m_runtime.injectReadinessStateForTest(readiness);
          updateVehicleRows();
          logFlightActionControlState("armed-ready");
          logSelectedActionState("armed-ready");
          logSelectedDroneViewState("armed-ready");
          logDroneListRowState("armed-ready");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
          updateVehicleRows();
          logSelectedActionState("manual-enabled");
          logSelectedDroneViewState("manual-enabled");
          logDroneListRowState("manual-enabled");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          MissionState mission;
          mission.droneId = m_runtime.targetDroneId();
          mission.missionId = "selected-action-test";
          mission.partId = "part-selected";
          mission.phase = "uploaded";
          mission.detail = "selected-action-test";
          mission.ack = "test";
          mission.transport = "test";
          mission.waypointsForwarded = "4";
          mission.waypointAcksAccepted = "4";
          mission.updatedMs = nowMilliseconds();
          m_runtime.injectMissionStateForTest(std::move(mission));
          updateVehicleRows();
          logSelectedActionState("mission-uploaded");
          logSelectedDroneViewState("mission-uploaded");
          logDroneListRowState("mission-uploaded");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Glib::signal_idle().connect_once([this] {
          logAppQualityState("flight-controls-final");
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
    if (m_autoDashboardPanelTest) {
      std::thread([this] {
        const bool refreshed = m_runtime.runOperatorDashboardSnapshotTest(std::chrono::seconds(30));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        Glib::signal_idle().connect_once([this, refreshed] {
          updateInspectorPanel();
          logOperatorDashboardPanelState("auto-dashboard-panel");
          const auto text = m_dashboardInspector.get_text();
          const bool textOk = text.find("Operator ready") != std::string::npos &&
                              text.find("Preflight") != std::string::npos &&
                              text.find("MAVLink") != std::string::npos &&
                              text.find("Parameters") != std::string::npos;
          const auto dashboard = m_runtime.operatorDashboardSnapshotForDrone(m_runtime.targetDroneId());
          const bool ok = refreshed && textOk &&
                          dashboard.preflightTotal >= 5 &&
                          dashboard.activeMavlinkMessageCount >= 2 &&
                          dashboard.parameterCount > 0;
          NDN_LOG_INFO("DASHBOARD_PANEL_RESULT ok=" << (ok ? "true" : "false")
                       << " refreshed=" << (refreshed ? "true" : "false")
                       << " text_ok=" << (textOk ? "true" : "false")
                       << " drone=" << dashboard.droneId
                       << " preflight=" << dashboard.preflightTotal
                       << " active_mavlink=" << dashboard.activeMavlinkMessageCount
                       << " parameters=" << dashboard.parameterCount);
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
  FlightSafetyGateState
  selectedFlightSafetyGateState() const
  {
    const auto droneId = m_runtime.targetDroneId();
    return FlightSafetyGateState::fromStates(droneId,
                                             m_runtime.readinessForDrone(droneId),
                                             m_runtime.safetyForDrone(droneId));
  }

  bool
  selectedFlightSafetyAllows(const std::string& action, std::string& reason) const
  {
    return selectedFlightSafetyGateState().actionAllowed(action, reason);
  }

  bool
  sendSelectedFlightCommandIfReady(const std::string& commandName, Fields params = {})
  {
    std::string reason;
    if (!selectedFlightSafetyAllows(commandName, reason)) {
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

  FlightActionControlState
  flightActionControlStateForSelected() const
  {
    return FlightActionControlState::fromGate(selectedFlightSafetyGateState());
  }

  void
  logFlightActionControlState(const std::string& phase) const
  {
    const auto state = flightActionControlStateForSelected();
    std::ostringstream os;
    os << "FLIGHT_ACTION_STATE phase=" << phase
       << " selected=" << state.selectedDrone
       << " has_readiness=" << (state.hasReadiness ? "true" : "false")
       << " has_safety=" << (state.hasSafety ? "true" : "false")
       << " safety_attention=" << (state.operatorAttention ? "true" : "false")
       << " link=" << state.linkState
       << " manual_state=" << state.manualControlState
       << " can_arm=" << (state.canArm ? "true" : "false")
       << " arm_reason=" << state.armReason
       << " can_takeoff=" << (state.canTakeoff ? "true" : "false")
       << " takeoff_reason=" << state.takeoffReason
       << " can_land=" << (state.canLand ? "true" : "false")
       << " land_reason=" << state.landReason
       << " can_manual=" << (state.canManualControl ? "true" : "false")
       << " manual_reason=" << state.manualControlReason
       << " can_panel=" << (state.canControlPanel ? "true" : "false")
       << " panel_reason=" << state.controlPanelReason
       << " emergency_stop=" << (state.canEmergencyStop ? "true" : "false")
       << " emergency_reason=" << state.emergencyStopReason;
    NDN_LOG_INFO(os.str());
  }

  void
  logManualSafetyProbe(const std::string& phase)
  {
    const auto droneId = m_runtime.targetDroneId();
    const auto fields = m_runtime.requestTelemetryStatusForDroneSync(
      droneId, std::chrono::milliseconds(2500));
    const auto safety = m_runtime.safetyForDrone(droneId);
    const auto flight = FlightActionControlState::fromGate(
      FlightSafetyGateState::fromStates(droneId, m_runtime.readinessForDrone(droneId), safety));
    std::ostringstream os;
    os << "MANUAL_SAFETY_STATE phase=" << phase
       << " drone=" << droneId
       << " telemetry=" << (fields.empty() ? "false" : "true");
    if (safety) {
      os << " " << safety->statusLine();
    }
    else {
      os << " Safety drone=" << droneId << " missing";
    }
    os << " " << flight.statusLine();
    NDN_LOG_INFO(os.str());
  }

  void
  updateFlightControlControls()
  {
    const auto state = flightActionControlStateForSelected();
    m_arm.set_sensitive(state.canArm);
    m_arm.set_tooltip_text(state.canArm ? "Arm selected drone" :
                           "Arm blocked: " + state.armReason);
    m_takeoff.set_sensitive(state.canTakeoff);
    m_takeoff.set_tooltip_text(state.canTakeoff ? "Take off selected drone" :
                               "Takeoff blocked: " + state.takeoffReason);
    m_land.set_sensitive(state.canLand);
    m_land.set_tooltip_text(state.canLand ? "Land selected drone" :
                            "Land blocked: " + state.landReason);
    if (m_controlMode && !state.canControlPanel) {
      setControlMode(false);
      m_status.set_text("Manual control disabled: drone=" + m_runtime.targetDroneId() +
                        " reason=" + state.controlPanelReason);
    }
    m_controlToggle.set_sensitive(state.canControlPanel || m_controlMode || state.canManualControl);
    m_controlToggle.set_tooltip_text(state.canControlPanel ? "Toggle manual control" :
                                     "Manual control blocked: " + state.controlPanelReason);
    m_emergencyStop.set_sensitive(state.canEmergencyStop);
    m_emergencyStop.set_tooltip_text(state.canEmergencyStop ?
                                     "Send emergency stop to Drone " + state.selectedDrone :
                                     "Emergency stop blocked: " + state.emergencyStopReason);
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
      if (!selectedFlightSafetyAllows("control_panel", reason)) {
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
    if (!selectedFlightSafetyAllows("manual_control", reason)) {
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
    m_lastDisplayedSeq = 0;
    m_lastDisplayedGeneration = m_streamGeneration;
    m_pendingFrameGeneration = m_streamGeneration;
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
    const auto control = videoControlStateForSelected();
    setPendingButtonStateLocked(control.canStart, control.canStop);
    if (control.displayActive) {
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
    m_lastDisplayedSeq = 0;
    m_lastDisplayedGeneration = 0;
    m_pendingFrameGeneration = 0;
    m_decodedFrames = 0;
    m_pendingClearFrame = true;
  }

  VideoControlState
  videoControlStateForSelected() const
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    return VideoControlState::fromStates(
      selectedDrone,
      m_runtime.videoForDrone(selectedDrone),
      m_runtime.isVideoDisplayActiveForDrone(selectedDrone));
  }

  void
  logVideoControlState(const std::string& phase)
  {
    const auto state = videoControlStateForSelected();
    std::ostringstream os;
    os << "VIDEO_SELECTION_STATE phase=" << phase
       << " selected=" << state.selectedDrone
       << " can_start=" << (state.canStart ? "true" : "false")
       << " can_stop=" << (state.canStop ? "true" : "false")
       << " start_button_sensitive=" << (m_start.get_sensitive() ? "true" : "false")
       << " stop_button_sensitive=" << (m_stop.get_sensitive() ? "true" : "false")
       << " remote_streaming=" << (state.remoteStreaming ? "true" : "false")
       << " display_active=" << (state.displayActive ? "true" : "false");
    NDN_LOG_INFO(os.str());
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
  pushEncodedChunk(std::vector<uint8_t> chunk, uint64_t seq, uint64_t elapsedMs,
                  const std::string& streamId, uint64_t streamSessionEpoch)
  {
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      const auto activeStreamId = m_runtime.activeVideoStreamId();
      if (!m_acceptFrames || !m_runtime.isVideoDisplayActiveForDrone(m_runtime.targetDroneId()) ||
          (!activeStreamId.empty() && streamId != activeStreamId) ||
          (streamSessionEpoch != 0 && !m_runtime.isCurrentVideoSessionPacket(streamId, streamSessionEpoch))) {
        return;
      }
      generation = m_streamGeneration;
    }
    Glib::signal_idle().connect_once([this, chunk = std::move(chunk), seq, elapsedMs, generation,
                                       streamId, streamSessionEpoch] {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        const auto activeStreamId = m_runtime.activeVideoStreamId();
        if (!m_acceptFrames ||
            generation != m_streamGeneration ||
            !m_runtime.isVideoDisplayActiveForDrone(m_runtime.targetDroneId()) ||
            (!activeStreamId.empty() && streamId != activeStreamId) ||
            (streamSessionEpoch != 0 && !m_runtime.isCurrentVideoSessionPacket(streamId, streamSessionEpoch))) {
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
            m_pendingFrameGeneration = generation;
          }
          ++m_decodedFrames;
          if (m_decodedFrames.load() <= 3 || m_decodedFrames.load() % 30 == 0) {
            NDN_LOG_INFO("GS_DECODED_FRAMES count=" << m_decodedFrames.load());
          }
          m_frameDispatcher.emit();
        }
      }
      catch (const Glib::Error& e) {
        NDN_LOG_WARN("GS_DECODER_ERROR " << e.what());
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
      const auto rowState = droneListRowState(m_droneIds[i], m_droneIds[i] == selectedDrone);
      label->set_text(compactDroneRowText(rowState));
      label->set_tooltip_text(rowState.rowText);
    }
    const auto view = selectedDroneViewState();
    m_mapMission.set_text(view.mapText);
    updateInspectorPanel();
    updateSelectedActionControls();
    refreshMapTile();
  }

  static std::string
  valueOr(const std::string& value, const std::string& fallback = "unknown")
  {
    return value.empty() ? fallback : value;
  }

  static void
  appendInspectorRow(std::ostringstream& os, const std::string& key,
                     const std::string& value)
  {
    os << key << ": " << valueOr(value) << "\n";
  }

  static std::string
  dashboardInspectorText(const UavOperatorDashboardSnapshot& dashboard)
  {
    std::ostringstream os;
    appendInspectorRow(os, "Drone", dashboard.droneId);
    appendInspectorRow(os, "Operator ready", dashboard.operatorReady() ? "yes" : "no");
    appendInspectorRow(os, "Telemetry", dashboard.telemetryFreshness);
    appendInspectorRow(os, "Readiness", dashboard.readiness + " (" +
                       dashboard.readinessReason + ")");
    appendInspectorRow(os, "Link / mode", dashboard.linkState + " / " +
                       dashboard.flightMode);
    appendInspectorRow(os, "Mission", dashboard.missionPhase);
    appendInspectorRow(os, "Video", dashboard.videoState);
    appendInspectorRow(os, "Parameters", dashboard.parameterCacheStatus + " " +
                       std::to_string(dashboard.parameterCount));
    appendInspectorRow(os, "Preflight", std::to_string(dashboard.preflightTotal) +
                       " checks, blocking=" +
                       std::to_string(dashboard.preflightBlockingFailures));
    appendInspectorRow(os, "MAVLink", std::to_string(dashboard.activeMavlinkMessageCount) +
                       "/" + std::to_string(dashboard.mavlinkMessageCount) +
                       " active");
    appendInspectorRow(os, "Actions", std::string("arm=") +
                       (dashboard.canArm ? "yes" : "no") +
                       " takeoff=" + (dashboard.canTakeoff ? "yes" : "no") +
                       " land=" + (dashboard.canLand ? "yes" : "no") +
                       " manual=" + (dashboard.canManualControl ? "yes" : "no") +
                       " e-stop=" + (dashboard.canEmergencyStop ? "yes" : "no"));
    return os.str();
  }

  static std::string
  oneLine(std::string text)
  {
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
  }

  void
  logOperatorDashboardPanelState(const std::string& phase) const
  {
    const auto dashboard = m_runtime.operatorDashboardSnapshotForDrone(m_runtime.targetDroneId());
    const auto text = dashboardInspectorText(dashboard);
    NDN_LOG_INFO("OPERATOR_DASHBOARD_PANEL_STATE phase=" << phase
                 << " selected=" << dashboard.droneId
                 << " operator_ready=" << (dashboard.operatorReady() ? "true" : "false")
                 << " telemetry=" << dashboard.telemetryFreshness
                 << " readiness=" << dashboard.readiness
                 << " preflight=" << dashboard.preflightTotal
                 << " blocking_failures=" << dashboard.preflightBlockingFailures
                 << " active_mavlink=" << dashboard.activeMavlinkMessageCount
                 << " parameters=" << dashboard.parameterCount
                 << " text=" << oneLine(text));
  }

  void
  updateInspectorPanel()
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    const auto telemetry = m_runtime.telemetryForDrone(selectedDrone);
    const auto readiness = m_runtime.readinessForDrone(selectedDrone);
    const auto video = m_runtime.videoForDrone(selectedDrone);
    const auto videoAdaptive = m_runtime.videoAdaptiveForDrone(selectedDrone);
    const auto mission = m_runtime.missionForDrone(selectedDrone);
    const auto command = m_runtime.commandForDrone(selectedDrone);
    const auto safety = m_runtime.safetyForDrone(selectedDrone);
    const auto missionProgress = m_runtime.missionProgressSnapshot();
    const auto missionPlan = m_runtime.missionPlanSnapshot();
    const auto missionPart = m_runtime.missionPartForDrone(selectedDrone);
    const auto authorityLease = m_runtime.operatorAuthorityLease();
    const auto snapshot = m_runtime.runtimeSnapshot();
    const auto* droneRuntime = snapshot.findDrone(selectedDrone);
    const auto nowMs = nowMilliseconds();
    const RuntimeCommandSnapshot* armCommand = nullptr;
    const RuntimeCommandSnapshot* landCommand = nullptr;
    const RuntimeCommandSnapshot* takeoffCommand = nullptr;
    if (droneRuntime) {
      const auto armIt = droneRuntime->commandStates.find("arm");
      if (armIt != droneRuntime->commandStates.end()) {
        armCommand = &armIt->second;
      }
      const auto takeoffIt = droneRuntime->commandStates.find("takeoff");
      if (takeoffIt != droneRuntime->commandStates.end()) {
        takeoffCommand = &takeoffIt->second;
      }
      const auto landIt = droneRuntime->commandStates.find("land");
      if (landIt != droneRuntime->commandStates.end()) {
        landCommand = &landIt->second;
      }
    }
    const auto availabilityText = [] (RuntimeAvailability availability) {
      switch (availability) {
      case RuntimeAvailability::Available:
        return "ready";
      case RuntimeAvailability::Unavailable:
        return "unavailable";
      case RuntimeAvailability::Unknown:
      default:
        return "unknown";
      }
    };
    const std::string cameraService = droneRuntime
      ? availabilityText(droneRuntime->cameraReady) : "unknown";
    const std::string mavlinkService = droneRuntime
      ? availabilityText(droneRuntime->flightControllerReady) : "unknown";
    const std::string missionService = droneRuntime
      ? availabilityText(droneRuntime->missionReady) : "unknown";
    const std::string recordingService = droneRuntime
      ? availabilityText(droneRuntime->videoReady) : "unknown";
    const std::string repoService = droneRuntime
      ? availabilityText(droneRuntime->repoReady) : "unknown";

    const auto dashboard = m_runtime.operatorDashboardSnapshotForDrone(selectedDrone);
    m_dashboardInspector.set_text(dashboardInspectorText(dashboard));

    std::ostringstream flight;
    appendInspectorRow(flight, "Drone", selectedDrone);
    appendInspectorRow(flight, "Backend", telemetry ? telemetry->flightControllerBackend : "unknown");
    appendInspectorRow(flight, "Available", telemetry ? telemetry->flightControllerAvailable : "unknown");
    appendInspectorRow(flight, "Ready", readiness ? readiness->flightControllerReady :
                       telemetry ? telemetry->flightControllerReady : "unknown");
    appendInspectorRow(flight, "Reason", readiness ? readiness->readinessReason :
                       telemetry ? telemetry->readinessReason : "unknown");
    appendInspectorRow(flight, "Armed", readiness ? readiness->armed :
                       telemetry ? telemetry->armed : "unknown");
    appendInspectorRow(flight, "Landed", readiness ? readiness->landedStateName :
                       telemetry ? telemetry->landedStateName : "unknown");
    appendInspectorRow(flight, "System", telemetry ? telemetry->systemStatusName : "unknown");
    appendInspectorRow(flight, "GPS fix", telemetry ? telemetry->gpsFixName : "unknown");
    appendInspectorRow(flight, "GPS sats", telemetry ? telemetry->gpsSatellitesVisible : "unknown");
    appendInspectorRow(flight, "EKF", telemetry ? telemetry->ekfReady : "unknown");
    m_flightInspector.set_text(flight.str());

    std::ostringstream telem;
    appendInspectorRow(telem, "Lat", telemetry ? telemetry->lat : "unknown");
    appendInspectorRow(telem, "Lon", telemetry ? telemetry->lon : "unknown");
    appendInspectorRow(telem, "Altitude", telemetry ? telemetry->altitudeM + " m" : "unknown");
    appendInspectorRow(telem, "Speed", telemetry ? telemetry->groundspeedMps + " m/s" : "unknown");
    appendInspectorRow(telem, "Battery", telemetry ? telemetry->batteryPercent + "%" : "unknown");
    appendInspectorRow(telem, "Voltage", telemetry ? telemetry->batteryVoltageV + " V" : "unknown");
    appendInspectorRow(telem, "Current", telemetry ? telemetry->batteryCurrentA + " A" : "unknown");
    appendInspectorRow(telem, "Heartbeat", telemetry ? telemetry->heartbeatSeen : "unknown");
    m_telemetryInspector.set_text(telem.str());

    std::ostringstream camera;
    appendInspectorRow(camera, "Available", video ? video->cameraAvailable :
                       telemetry ? telemetry->cameraAvailable : "unknown");
    appendInspectorRow(camera, "Source", video ? video->source :
                       telemetry ? telemetry->cameraSource : "unknown");
    appendInspectorRow(camera, "Reason", video ? video->cameraReason :
                       telemetry ? telemetry->cameraReason : "unknown");
    appendInspectorRow(camera, "Capture", video ? video->capture :
                       telemetry ? telemetry->capture : "unknown");
    appendInspectorRow(camera, "Recording", video ? video->recording :
                       telemetry ? telemetry->recording : "unknown");
    if (video) {
      appendInspectorRow(camera, "Chunks", std::to_string(video->recordingChunks));
      appendInspectorRow(camera, "Bytes", std::to_string(video->recordingBytes));
    }
    m_cameraInspector.set_text(camera.str());

    std::ostringstream videoText;
    appendInspectorRow(videoText, "State", video ? video->status : "unknown");
    appendInspectorRow(videoText, "Encoding", video ? video->encoding : "unknown");
    appendInspectorRow(videoText, "Bitrate", video ?
                       std::to_string(video->acceptedBitrateKbps) + "/" +
                       std::to_string(video->requestedBitrateKbps) + " kbps" : "unknown");
    appendInspectorRow(videoText, "Frame", video ?
                       std::to_string(video->acceptedFrameWidth) + " px @ " +
                       std::to_string(video->fps) + " fps" : "unknown");
    appendInspectorRow(videoText, "Packets", video ?
                       std::to_string(video->streamPacketsPublished) : "unknown");
    appendInspectorRow(videoText, "Frames", video ?
                       std::to_string(video->framesPublished) + " pub / " +
                       std::to_string(video->decodedFrames) + " dec" : "unknown");
    if (videoAdaptive) {
      appendInspectorRow(videoText, "Adaptive", videoAdaptive->state);
      appendInspectorRow(videoText, "Window", std::to_string(videoAdaptive->window));
      appendInspectorRow(videoText, "Pressure", videoAdaptive->primaryPressure);
      appendInspectorRow(videoText, "Suggest", videoAdaptive->bitrateAction + " " +
                         std::to_string(videoAdaptive->suggestedBitrateKbps) + " kbps");
    }
    m_videoInspector.set_text(videoText.str());

    std::string selectedControlReason;
    const bool selectedControlAllowed = authorityLease.allowsCommand(
      selectedDrone, "land", nowMs, selectedControlReason);
    std::string selectedTelemetryReason;
    const bool selectedTelemetryAllowed = authorityLease.allowsCommand(
      selectedDrone, "telemetry", nowMs, selectedTelemetryReason);
    std::ostringstream authorityText;
    appendInspectorRow(authorityText, "Operator", authorityLease.operatorId);
    appendInspectorRow(authorityText, "Scope", authorityLease.scope);
    appendInspectorRow(authorityText, "Lease drone", authorityLease.droneId);
    appendInspectorRow(authorityText, "Lease id", authorityLease.leaseId);
    appendInspectorRow(authorityText, "Expires", authorityLease.expiresMs == 0 ?
                       "never" : std::to_string(authorityLease.expiresMs));
    appendInspectorRow(authorityText, "Revoked", authorityLease.revoked ? "true" : "false");
    appendInspectorRow(authorityText, "Refresh", m_operatorAuthorityRefreshIntervalMs == 0 ?
                       "manual" : std::to_string(m_operatorAuthorityRefreshIntervalMs) + " ms");
    appendInspectorRow(authorityText, "Selected control",
                       std::string(selectedControlAllowed ? "allowed" : "blocked") +
                       " (" + selectedControlReason + ")");
    appendInspectorRow(authorityText, "Selected telemetry",
                       std::string(selectedTelemetryAllowed ? "allowed" : "blocked") +
                       " (" + selectedTelemetryReason + ")");
    size_t alertIndex = 1;
    for (auto it = snapshot.operatorAuthorityAlerts.rbegin();
         it != snapshot.operatorAuthorityAlerts.rend() && alertIndex <= 3; ++it, ++alertIndex) {
      const auto ageMs = nowMs > it->updatedMs ? nowMs - it->updatedMs : 0;
      appendInspectorRow(authorityText, "Alert " + std::to_string(alertIndex),
                         it->statusLine() + " age_ms=" + std::to_string(ageMs));
    }
    m_authorityInspector.set_text(authorityText.str());

    std::ostringstream missionText;
    appendInspectorRow(missionText, "Mission", mission ? mission->phase : "idle");
    appendInspectorRow(missionText, "Detail", mission ? mission->detail : "idle");
    appendInspectorRow(missionText, "Task", missionPlan ? missionPlan->taskId : "none");
    appendInspectorRow(missionText, "Part", missionPart ? missionPart->id : "none");
    appendInspectorRow(missionText, "Waypoints", missionPart ?
                       std::to_string(missionPart->waypoints.size()) : "0");
    appendInspectorRow(missionText, "Progress", missionProgress ? missionProgress->phase : "idle");
    appendInspectorRow(missionText, "Safety", safety ? safety->detail : "idle");
    appendInspectorRow(missionText, "Link", safety ? safety->linkState :
                       telemetry ? telemetry->linkState : "unknown");
    appendInspectorRow(missionText, "Manual", safety ? safety->manualControlState :
                       telemetry ? telemetry->manualControlState : "idle");
    if (command) {
      appendInspectorRow(missionText, "Last cmd", command->command + " " + command->accepted);
    }
    if (armCommand != nullptr) {
      const auto ageMs = nowMs > armCommand->updatedMs ? nowMs - armCommand->updatedMs : 0;
      const auto armLatency = armCommand->rttMs > 0 ?
        (std::to_string(armCommand->rttMs) + " ms") :
        (armCommand->timeoutMs > 0 ? (std::to_string(armCommand->timeoutMs) + " ms timeout") : "n/a");
      appendInspectorRow(missionText, "Arm lifecycle", to_string(armCommand->lifecycle));
      appendInspectorRow(missionText, "Arm detail", armCommand->detail);
      appendInspectorRow(missionText, "Arm latency", armLatency);
      appendInspectorRow(missionText, "Arm updated", std::to_string(ageMs) + " ms ago");
    }
    if (takeoffCommand != nullptr) {
      const auto ageMs = nowMs > takeoffCommand->updatedMs ? nowMs - takeoffCommand->updatedMs : 0;
      const auto takeoffLatency = takeoffCommand->rttMs > 0 ?
        (std::to_string(takeoffCommand->rttMs) + " ms") :
        (takeoffCommand->timeoutMs > 0 ? (std::to_string(takeoffCommand->timeoutMs) + " ms timeout") : "n/a");
      appendInspectorRow(missionText, "Takeoff lifecycle", to_string(takeoffCommand->lifecycle));
      appendInspectorRow(missionText, "Takeoff detail", takeoffCommand->detail);
      appendInspectorRow(missionText, "Takeoff latency", takeoffLatency);
      appendInspectorRow(missionText, "Takeoff updated", std::to_string(ageMs) + " ms ago");
    }
    if (landCommand != nullptr) {
      const auto ageMs = nowMs > landCommand->updatedMs ? nowMs - landCommand->updatedMs : 0;
      const auto landLatency = landCommand->rttMs > 0 ?
        (std::to_string(landCommand->rttMs) + " ms") :
        (landCommand->timeoutMs > 0 ? (std::to_string(landCommand->timeoutMs) + " ms timeout") : "n/a");
      appendInspectorRow(missionText, "Land lifecycle", to_string(landCommand->lifecycle));
      appendInspectorRow(missionText, "Land detail", landCommand->detail);
      appendInspectorRow(missionText, "Land latency", landLatency);
      appendInspectorRow(missionText, "Land updated", std::to_string(ageMs) + " ms ago");
    }
    m_missionInspector.set_text(missionText.str());

    std::ostringstream missionDetail;
    const std::string selectedMissionId = mission && !mission->missionId.empty() &&
      mission->missionId != "none" && mission->missionId != "unknown" ? mission->missionId :
      missionProgress && !missionProgress->taskId.empty() &&
      missionProgress->taskId != "none" ? missionProgress->taskId :
      missionPlan && !missionPlan->taskId.empty() && missionPlan->taskId != "none" ?
      missionPlan->taskId : "none";
    appendInspectorRow(missionDetail, "Task ID", selectedMissionId);
    const std::string assignedDrones = missionProgress && !missionProgress->drones.empty() &&
      missionProgress->drones != "none" && missionProgress->drones != "unknown" ?
      missionProgress->drones : std::string{};
    appendInspectorRow(missionDetail, "Drones", assignedDrones.empty() ?
      (missionPart ? missionPart->assignedDrone : "none") : assignedDrones);
    const size_t totalMissionWaypoints = missionPlan ? std::accumulate(
      missionPlan->parts.begin(), missionPlan->parts.end(), size_t{0},
      [] (size_t sum, const MissionPart& part) { return sum + part.waypoints.size(); }) : size_t{0};
    appendInspectorRow(missionDetail, "Waypoints", std::to_string(totalMissionWaypoints));
    appendInspectorRow(missionDetail, "Progress", missionProgress ? missionProgress->statusLine() :
                       mission ? mission->statusLine() : "idle");
    appendInspectorRow(missionDetail, "State", mission ? mission->phase : (missionProgress ?
                       missionProgress->phase : (missionPart ? missionPart->statusLine() : "idle")));
    appendInspectorRow(missionDetail, "Parts", std::to_string(missionPlan ? missionPlan->parts.size() : 0));
    if (missionPlan && !missionPlan->returnHomePlanned) {
      appendInspectorRow(missionDetail, "Return home", "no");
    }
    else if (missionPlan && missionPlan->returnHomePlanned) {
      appendInspectorRow(missionDetail, "Return home", "yes");
    }
    m_missionDetail.set_text(missionDetail.str());

    const auto catalog = m_runtime.catalogForDrone(selectedDrone);
    std::ostringstream catalogText;
    appendInspectorRow(catalogText, "Products",
                       catalog ? std::to_string(catalog->totalProducts()) : "0");
    appendInspectorRow(catalogText, "Repo objects",
                       catalog ? std::to_string(catalog->repoObjects) : "0");
    appendInspectorRow(catalogText, "Recordings",
                       catalog ? std::to_string(catalog->recordingProducts) : "0");
    appendInspectorRow(catalogText, "Telemetry logs",
                       catalog ? std::to_string(catalog->telemetryLogProducts) : "0");
    appendInspectorRow(catalogText, "Detections",
                       catalog ? std::to_string(catalog->detectionProducts) : "0");
    appendInspectorRow(catalogText, "Mission logs",
                       catalog ? std::to_string(catalog->missionLogProducts) : "0");
    appendInspectorRow(catalogText, "Bytes",
                       catalog ? std::to_string(catalog->totalBytes) : "0");
    appendInspectorRow(catalogText, "Source", catalog ? catalog->sourceRepo : "unknown");
    appendInspectorRow(catalogText, "Latest", catalog ?
                       catalog->latestProductType + ":" + catalog->latestObjectPrefix : "none");
    m_catalogInspector.set_text(catalogText.str());

    const auto parameters = m_runtime.parameterSnapshotForDrone(selectedDrone);
    std::ostringstream parameterText;
    appendInspectorRow(parameterText, "Source", parameters ? parameters->source : "unknown");
    appendInspectorRow(parameterText, "Firmware", parameters ? parameters->firmware : "unknown");
    appendInspectorRow(parameterText, "Vehicle", parameters ? parameters->vehicleType : "unknown");
    appendInspectorRow(parameterText, "Modes", parameters ? parameters->flightModes : "unknown");
    appendInspectorRow(parameterText, "Count",
                       parameters ? std::to_string(parameters->parameterCount) : "0");
    appendInspectorRow(parameterText, "Complete",
                       parameters ? std::to_string(parameters->completePercent) + "%" : "0%");
    appendInspectorRow(parameterText, "NAV_RCL_ACT",
                       parameters && parameters->parameters.count("NAV_RCL_ACT") ?
                       parameters->parameters.at("NAV_RCL_ACT") : "unknown");
    appendInspectorRow(parameterText, "COM_RC_LOSS_T",
                       parameters && parameters->parameters.count("COM_RC_LOSS_T") ?
                       parameters->parameters.at("COM_RC_LOSS_T") : "unknown");
    m_parameterInspector.set_text(parameterText.str());

    const auto functionality = m_runtime.functionalitySnapshotForSelectedDrone();
    std::ostringstream functionalityText;
    appendInspectorRow(functionalityText, "Mission editor", functionality.missionEditor);
    appendInspectorRow(functionalityText, "Per-drone review", functionality.perDroneMissionReview);
    appendInspectorRow(functionalityText, "Mission files", functionality.persistentMissionFiles);
    appendInspectorRow(functionalityText, "Recording/logs", functionality.recordingLogBrowsing);
    appendInspectorRow(functionalityText, "Parameter/status", functionality.parameterStatusInspection);
    appendInspectorRow(functionalityText, "Detection display", functionality.objectDetectionDisplay);
    appendInspectorRow(functionalityText, "Multi-drone select", functionality.multiDroneServiceSelection);
    appendInspectorRow(functionalityText, "Implemented",
                       std::to_string(functionality.implementedCapabilityCount()) + "/7");
    appendInspectorRow(functionalityText, "Weak/missing", functionality.missingOrLimitedCapabilities());
    m_functionalityInspector.set_text(functionalityText.str());

    const auto practicality = m_runtime.practicalitySnapshotForSelectedDrone();
    std::ostringstream practicalityText;
    appendInspectorRow(practicalityText, "Preflight", practicality.preflightSummary);
    appendInspectorRow(practicalityText, "Hardware notes", practicality.hardwareCompatibilityNotes);
    appendInspectorRow(practicalityText, "Camera diagnostics", practicality.cameraDiagnostics);
    appendInspectorRow(practicalityText, "FC diagnostics", practicality.flightControllerDiagnostics);
    appendInspectorRow(practicalityText, "Config validation", practicality.configValidation);
    appendInspectorRow(practicalityText, "Identity guidance", practicality.identityCertificateGuidance);
    appendInspectorRow(practicalityText, "Operator workflow", practicality.operatorWorkflowGuidance);
    appendInspectorRow(practicalityText, "Implemented",
                       std::to_string(practicality.practicalCapabilityCount()) + "/7");
    appendInspectorRow(practicalityText, "Weak/missing", practicality.missingOrLimitedCapabilities());
    m_practicalityInspector.set_text(practicalityText.str());

    const auto stability = m_runtime.stabilitySnapshotForSelectedDrone();
    std::ostringstream stabilityText;
    appendInspectorRow(stabilityText, "Command timeout", stability.commandTimeoutHandling);
    appendInspectorRow(stabilityText, "Stop Video", stability.stopVideoIdempotence);
    appendInspectorRow(stabilityText, "Stream session", stability.streamSessionGuard);
    appendInspectorRow(stabilityText, "Frame sequence", stability.frameSequenceGuard);
    appendInspectorRow(stabilityText, "Adaptive video", stability.adaptiveVideoPressure);
    appendInspectorRow(stabilityText, "Telemetry", stability.telemetryFreshness);
    appendInspectorRow(stabilityText, "Manual neutral", stability.manualNeutralFallback);
    appendInspectorRow(stabilityText, "Long duration", stability.longDurationProfiles);
    appendInspectorRow(stabilityText, "Implemented",
                       std::to_string(stability.stableCapabilityCount()) + "/8");
    appendInspectorRow(stabilityText, "Weak/missing", stability.missingOrLimitedCapabilities());
    m_stabilityInspector.set_text(stabilityText.str());

    std::ostringstream commandHistory;
    if (droneRuntime && !droneRuntime->commandHistory.empty()) {
      size_t index = 1;
      for (auto it = droneRuntime->commandHistory.rbegin();
           it != droneRuntime->commandHistory.rend() && index <= 10; ++it, ++index) {
        const auto ageMs = nowMs > it->updatedMs ? nowMs - it->updatedMs : 0;
        const auto latencyLabel = it->rttMs > 0 ? "RTT"
                                                : (it->timeoutMs > 0 ? "timeout" : "n/a");
        const auto latencyMs = it->rttMs > 0 ? it->rttMs : it->timeoutMs;
        commandHistory << index << ") " << it->command << " "
                       << to_string(it->lifecycle) << "\n";
        commandHistory << "   " << it->detail << "\n";
        commandHistory << "   " << latencyLabel << ": " << latencyMs << " ms\n";
        commandHistory << "   " << ageMs << " ms ago\n";
      }
    }
    else {
      commandHistory << "No command history.";
    }
    m_commandHistory.set_text(commandHistory.str());

    std::ostringstream services;
    services << (m_pendingLinkStatus.empty() ? "Link RTT: waiting" : m_pendingLinkStatus) << "\n";
    services << "Service availability:\n";
    services << "  camera=" << cameraService
             << "  mavlink=" << mavlinkService
             << "  mission=" << missionService
             << "  recording=" << recordingService
             << "  repo=" << repoService << "\n";
    services << "Services for Drone " << selectedDrone << ":\n";
    services << m_runtime.serviceCatalogForDrone(selectedDrone);
    m_services.set_text(services.str());
  }

  void
  updateMissionControls()
  {
    const auto state = missionControlState();
    m_patrol.set_sensitive(state.canUpload);
    m_patrol.set_tooltip_text(state.canUpload ? "Upload cooperative patrol mission" :
                              "Upload blocked: " + state.uploadReason);
    m_startMission.set_sensitive(state.canStart);
    m_startMission.set_tooltip_text(state.canStart ? "Start uploaded patrol mission" :
                                    "Start blocked: " + state.startReason);
    m_stopPatrol.set_sensitive(state.canStop);
    m_stopPatrol.set_tooltip_text(state.canStop ? "Stop active patrol mission" :
                                  "Stop blocked: " + state.stopReason);
  }

  std::optional<FlightSafetyGateState>
  flightSafetyGateForDrone(const std::string& droneId) const
  {
    const auto readiness = m_runtime.readinessForDrone(droneId);
    if (!readiness) {
      return std::nullopt;
    }
    return FlightSafetyGateState::fromStates(droneId, readiness, m_runtime.safetyForDrone(droneId));
  }

  MissionStartGateState
  missionStartGateForDrone(const std::string& droneId) const
  {
    return MissionStartGateState::fromStates(droneId, m_runtime.missionForDrone(droneId),
                                             flightSafetyGateForDrone(droneId));
  }

  std::vector<std::string>
  missionStartEligibleDrones() const
  {
    std::vector<std::string> out;
    for (const auto& droneId : m_droneIds) {
      if (missionStartGateForDrone(droneId).canStart) {
        out.push_back(droneId);
      }
    }
    return out;
  }

  MissionControlState
  missionControlState() const
  {
    std::vector<MissionStartGateState> gates;
    gates.reserve(m_droneIds.size());
    for (const auto& droneId : m_droneIds) {
      gates.push_back(missionStartGateForDrone(droneId));
    }
    return MissionControlState::fromStates(gates,
                                           m_runtime.missionProgressSnapshot(),
                                           m_patrolUploadInFlight.load(),
                                           m_missionStartInFlight.load(),
                                           m_patrolStopInFlight.load());
  }

  DroneListRowState
  droneListRowState(const std::string& droneId, bool selected) const
  {
    const auto telemetry = m_runtime.telemetryForDrone(droneId);
    const auto readiness = m_runtime.readinessForDrone(droneId);
    const auto mission = m_runtime.missionForDrone(droneId);
    const auto video = m_runtime.videoForDrone(droneId);
    const auto videoAdaptive = m_runtime.videoAdaptiveForDrone(droneId);
    const auto command = m_runtime.commandForDrone(droneId);
    const auto safety = m_runtime.safetyForDrone(droneId);
    const auto missionPart = m_runtime.missionPartForDrone(droneId);
    const auto progress = m_runtime.missionProgressSnapshot();
    const auto snapshot = m_runtime.runtimeSnapshot();
    const auto* droneRuntime = snapshot.findDrone(droneId);
    const auto availabilityText = [] (RuntimeAvailability availability) {
      switch (availability) {
      case RuntimeAvailability::Available:
        return "ready";
      case RuntimeAvailability::Unavailable:
        return "unavailable";
      case RuntimeAvailability::Unknown:
      default:
        return "unknown";
      }
    };
    const std::string cameraService = droneRuntime
      ? availabilityText(droneRuntime->cameraReady) : "unknown";
    const std::string mavlinkService = droneRuntime
      ? availabilityText(droneRuntime->flightControllerReady) : "unknown";
    const std::string missionService = droneRuntime
      ? availabilityText(droneRuntime->missionReady) : "unknown";
    const std::string recordingService = droneRuntime
      ? availabilityText(droneRuntime->videoReady) : "unknown";
    const std::string repoService = droneRuntime
      ? availabilityText(droneRuntime->repoReady) : "unknown";
    return DroneListRowState::fromStates(droneId, selected, telemetry, readiness,
                                         mission, video, videoAdaptive, command,
                                         safety, progress,
                                         missionPart,
                                         cameraService, mavlinkService,
                                         missionService, recordingService,
                                         repoService);
  }

  static std::string
  compactDroneRowText(const DroneListRowState& state)
  {
    std::ostringstream os;
    os << (state.selected ? "● " : "○ ") << "Drone " << state.droneId
       << (state.selected ? " active" : " standby");
    if (state.hasReadiness || state.hasTelemetry) {
      os << "\n" << state.readiness
         << " arm=" << state.armed
         << " gps=" << state.gps
         << " bat=" << state.battery;
    }
    if (state.hasTelemetry || state.hasVideo || state.hasSafety) {
      os << "\nvideo=" << state.video
         << " safe=" << state.safety;
    }
    if (state.missionPartId != "none") {
      os << "\nmission segment=" << state.missionPartId
         << " " << state.missionSegmentState;
    }
    os << "\nservices: camera=" << state.serviceCamera
       << " mavlink=" << state.serviceMavlink
       << " mission=" << state.serviceMission
       << " recording=" << state.serviceRecording
       << " repo=" << state.serviceRepo;
    return os.str();
  }

  void
  logDroneListRowState(const std::string& phase) const
  {
    const auto state = droneListRowState(m_runtime.targetDroneId(), true);
    std::ostringstream os;
    os << "DRONE_ROW_STATE phase=" << phase
       << " selected=" << state.droneId
       << " has_telemetry=" << (state.hasTelemetry ? "true" : "false")
       << " readiness=" << state.readiness
       << " armed=" << state.armed
       << " gps=" << state.gps
       << " mission=" << state.mission
       << " mission_progress=" << state.missionProgress
       << " mission_part=" << state.missionPartId
       << " mission_segment_state=" << state.missionSegmentState
       << " video=" << state.video
       << " video_adaptive=" << state.videoAdaptive
       << " safety=" << state.safety
       << " text=" << state.rowText;
    NDN_LOG_INFO(os.str());
  }

  void
  logMissionControlState(const std::string& phase) const
  {
    const auto state = missionControlState();
    std::ostringstream os;
    os << "MISSION_CONTROL_STATE phase=" << phase
       << " can_upload=" << (state.canUpload ? "true" : "false")
       << " upload_reason=" << state.uploadReason
       << " can_start=" << (state.canStart ? "true" : "false")
       << " start_reason=" << state.startReason
       << " can_stop=" << (state.canStop ? "true" : "false")
       << " stop_reason=" << state.stopReason
       << " upload_pending=" << (state.uploadPending ? "true" : "false")
       << " start_pending=" << (state.startPending ? "true" : "false")
       << " stop_pending=" << (state.stopPending ? "true" : "false")
       << " startable_count=" << state.startableCount
       << " start_eligible_count=" << state.startEligibleCount
       << " start_blocked_count=" << state.startBlockedCount
       << " has_uploaded=" << (state.hasUploaded ? "true" : "false")
       << " has_executing=" << (state.hasExecuting ? "true" : "false")
       << " has_stopping=" << (state.hasStopping ? "true" : "false")
       << " progress_phase=" << state.progressPhase
       << " progress_active=" << (state.progressActive ? "true" : "false")
       << " progress_needs_compensation=" << (state.progressNeedsCompensation ? "true" : "false")
       << " progress_complete=" << (state.progressComplete ? "true" : "false")
       << " progress_failed=" << (state.progressFailed ? "true" : "false")
       << " start_eligible=" << state.startEligible
       << " start_blocked=" << state.startBlocked
       << " phases=" << state.phases;
    NDN_LOG_INFO(os.str());
  }

  void
  logAppQualityState(const std::string& phase) const
  {
    const auto functionality = m_runtime.functionalitySnapshotForSelectedDrone();
    const auto practicality = m_runtime.practicalitySnapshotForSelectedDrone();
    const auto stability = m_runtime.stabilitySnapshotForSelectedDrone();
    std::ostringstream os;
    os << "UAV_APP_QUALITY_STATE phase=" << phase
       << " selected=" << m_runtime.targetDroneId()
       << " functionality={" << functionality.statusLine() << "}"
       << " practicality={" << practicality.statusLine() << "}"
       << " stability={" << stability.statusLine() << "}";
    NDN_LOG_INFO(os.str());
  }

  SelectedActionState
  selectedActionState() const
  {
    return SelectedActionState::fromStates(m_runtime.targetDroneId(),
                                           flightActionControlStateForSelected(),
                                           missionControlState(),
                                           m_controlMode,
                                           m_manualActive.load());
  }

  void
  logSelectedActionState(const std::string& phase) const
  {
    const auto state = selectedActionState();
    std::ostringstream os;
    os << "SELECTED_ACTION_STATE phase=" << phase
       << " selected=" << state.selectedDrone
       << " can_arm=" << (state.flight.canArm ? "true" : "false")
       << " can_takeoff=" << (state.flight.canTakeoff ? "true" : "false")
       << " can_land=" << (state.flight.canLand ? "true" : "false")
       << " can_manual=" << (state.flight.canManualControl ? "true" : "false")
       << " can_panel=" << (state.flight.canControlPanel ? "true" : "false")
       << " mission_can_start=" << (state.mission.canStart ? "true" : "false")
       << " mission_start_reason=" << state.mission.startReason
       << " mission_can_stop=" << (state.mission.canStop ? "true" : "false")
       << " mission_stop_reason=" << state.mission.stopReason
       << " mission_phases=" << state.mission.phases
       << " mission_progress=" << state.mission.progressPhase
       << " manual_mode=" << (state.manualMode ? "true" : "false")
       << " manual_active=" << (state.manualInputActive ? "true" : "false")
       << " emergency_stop=" << (state.emergencyStopAvailable ? "true" : "false");
    NDN_LOG_INFO(os.str());
  }

  void
  updateSelectedActionControls()
  {
    const auto state = selectedActionState();
    m_patrol.set_sensitive(state.mission.canUpload);
    m_patrol.set_tooltip_text(state.mission.canUpload ? "Upload cooperative patrol mission" :
                              "Upload blocked: " + state.mission.uploadReason);
    m_startMission.set_sensitive(state.mission.canStart);
    m_startMission.set_tooltip_text(state.mission.canStart ? "Start uploaded patrol mission" :
                                    "Start blocked: " + state.mission.startReason);
    m_stopPatrol.set_sensitive(state.mission.canStop);
    m_stopPatrol.set_tooltip_text(state.mission.canStop ? "Stop active patrol mission" :
                                  "Stop blocked: " + state.mission.stopReason);
    m_arm.set_sensitive(state.flight.canArm);
    m_arm.set_tooltip_text(state.flight.canArm ? "Arm selected drone" :
                           "Arm blocked: " + state.flight.armReason);
    m_takeoff.set_sensitive(state.flight.canTakeoff);
    m_takeoff.set_tooltip_text(state.flight.canTakeoff ? "Take off selected drone" :
                               "Takeoff blocked: " + state.flight.takeoffReason);
    m_land.set_sensitive(state.flight.canLand);
    m_land.set_tooltip_text(state.flight.canLand ? "Land selected drone" :
                            "Land blocked: " + state.flight.landReason);
    if (m_controlMode && !state.flight.canControlPanel) {
      setControlMode(false);
      m_status.set_text("Manual control disabled: drone=" + state.selectedDrone +
                        " reason=" + state.flight.controlPanelReason);
    }
    m_controlToggle.set_sensitive(state.flight.canControlPanel ||
                                  state.manualMode ||
                                  state.flight.canManualControl);
    m_controlToggle.set_tooltip_text(state.flight.canControlPanel ? "Toggle manual control" :
                                     "Manual control blocked: " + state.flight.controlPanelReason);
    m_emergencyStop.set_sensitive(state.emergencyStopAvailable);
    m_emergencyStop.set_tooltip_text(state.emergencyStopAvailable ?
                                     "Send emergency stop to Drone " + state.selectedDrone :
                                     "Select a drone before emergency stop");
    m_refreshAuthority.set_sensitive(!m_authorityRefreshInFlight.load());
    m_refreshAuthority.set_tooltip_text(
      m_authorityRefreshInFlight.load() ? "Operator authority refresh is already running" :
      "Query the authority issuer for revocation status of the active lease");
  }

  void
  triggerAuthorityLeaseRefresh(const std::string& source)
  {
    bool expected = false;
    if (!m_authorityRefreshInFlight.compare_exchange_strong(expected, true)) {
      m_status.set_text("Operator authority refresh already in progress");
      return;
    }
    m_refreshAuthority.set_sensitive(false);
    m_status.set_text("Refreshing operator authority lease...");
    std::thread([this, source] {
      std::string reason;
      Fields fields;
      const bool revoked = m_runtime.refreshOperatorAuthorityLeaseFromIssuer(
        m_runtime.config().groundStationIdentity, std::chrono::seconds(5), reason, &fields);
      Glib::signal_idle().connect_once([this, source, revoked, reason, fields] {
        m_authorityRefreshInFlight = false;
        m_status.set_text(std::string("Authority refresh ") + source +
                          ": " + (revoked ? "lease revoked" : "not revoked") +
                          " (" + reason + ")");
        NDN_LOG_INFO("AUTHORITY_GUI_REFRESH source=" << source
                     << " revoked=" << (revoked ? "true" : "false")
                     << " reason=" << reason
                     << " revoker_operator=" << fieldOr(fields, "revoker_operator", "none"));
        updateVehicleRows();
      });
    }).detach();
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
  formatTag(const std::string& key, const std::string& value)
  {
    return "[" + key + ": " + value + "]";
  }

  std::string
  mapTextForTelemetry(const TelemetryState& telemetry,
                      const std::optional<MissionState>& mission,
                      const std::string& selectedDrone,
                      const std::optional<ReadinessState>& readiness = std::nullopt,
                      const std::optional<VideoState>& video = std::nullopt,
                      const std::optional<FlightCommandState>& command = std::nullopt,
                      const std::optional<SafetyState>& safety = std::nullopt) const
  {
    std::ostringstream text;
    const auto missionPhase = mission ? mission->phase : "idle";
    const auto missionDetail = mission ? mission->detail : "idle";
    const auto telemetryFreshness = telemetry.telemetryFreshnessLabel();
    auto missionLatLon = telemetry.lat + ", " + telemetry.lon;
    try {
      missionLatLon = formatLatLon(std::stod(telemetry.lat), std::stod(telemetry.lon));
    }
    catch (const std::exception&) {
    }
    text << formatTag("selected-drone", "Drone " + selectedDrone) << " "
         << formatTag("freshness", telemetryFreshness) << "\n";
    text << formatTag("position", missionLatLon) << " "
         << formatTag("alt_m", telemetry.altitudeM) << " "
         << formatTag("spd_mps", telemetry.groundspeedMps) << "\n";
    text << formatTag("mission", missionPhase) << " "
         << formatTag("mission_detail", missionDetail) << "\n";
    if (mission) {
      text << formatTag("mission_gate_start", mission->isStartable() ? "ready" : "blocked") << " "
           << formatTag("mission_gate_stop", mission->isStoppable() ? "ready" : "blocked") << " "
           << formatTag("mission_busy", mission->isBusyForAssignment() ? "yes" : "no") << "\n";
    }
    if (readiness) {
      text << formatTag("flight", readiness->readiness) << " "
           << formatTag("flight_reason", readiness->readinessReason) << "\n"
           << formatTag("arm", readiness->readyForArm() ? "ready" : "blocked") << " "
           << formatTag("takeoff", readiness->readyForTakeoff() ? "ready" : "blocked") << " "
           << formatTag("land", readiness->readyForLand() ? "ready" : "blocked") << "\n";
    }
    if (video) {
      text << formatTag("video", video->status) << " "
           << formatTag("capture", video->capture) << " "
           << formatTag("camera", video->cameraAvailable) << " "
           << formatTag("packets", std::to_string(video->streamPacketsPublished)) << "\n";
    }
    if (command && command->command != "none") {
      text << formatTag("command", command->command) << " "
           << formatTag("cmd_accept", command->accepted) << " "
           << formatTag("ack", command->ackResult) << "\n";
    }
    if (safety) {
      text << formatTag("link", safety->linkState) << " "
           << formatTag("manual", safety->manualControlState) << " "
           << formatTag("attention", safety->needsOperatorAttention() ? "yes" : "no") << "\n";
    }
    text << formatTag("map_tools", std::string("click=add_wp; drag=pan; wheel=zoom; follow=") +
                             (m_mapFollowSelected.get_active() ? "on" : "off"));
    return text.str();
  }

  std::string
  mapWorkspaceStatusText(const std::string& event, const std::string& selectedDrone,
                         const std::string& action) const
  {
    std::ostringstream text;
    text << formatTag("mission", "workspace") << " "
         << formatTag("selected-drone", "Drone " + selectedDrone) << "\n";
    text << formatTag("center", formatLatLon(m_mapCenterLat, m_mapCenterLon)) << " "
         << formatTag("waypoints", std::to_string(m_planWaypoints.size())) << "\n";
    text << formatTag("event", event) << " "
         << formatTag("action", action) << "\n";
    text << formatTag("map_tools", std::string("click=add_wp; drag=pan; wheel=zoom; follow=") +
                             (m_mapFollowSelected.get_active() ? "on" : "off"));
    return text.str();
  }

  void
  maybeFollowSelectedDrone(const std::string& statusDrone)
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    if (!m_mapFollowSelected.get_active() || statusDrone != selectedDrone) {
      return;
    }

    const auto telemetry = m_runtime.telemetryForDrone(selectedDrone);
    if (!telemetry) {
      return;
    }
    try {
      m_mapCenterLat = std::stod(telemetry->lat);
      m_mapCenterLon = std::stod(telemetry->lon);
    }
    catch (const std::exception&) {
      return;
    }
    m_pendingMap = mapWorkspaceStatusText("follow-selected",
                                          selectedDrone,
                                          "follow selected drone");
  }

  void
  applyFollowSelectedToMap(const std::string& selectedDrone)
  {
    if (!m_mapFollowSelected.get_active()) {
      m_mapMission.set_text(mapWorkspaceStatusText("follow-selected-off",
                                                   selectedDrone,
                                                   "map follow disabled"));
      return;
    }
    const auto telemetry = m_runtime.telemetryForDrone(selectedDrone);
    if (telemetry) {
      try {
        m_mapCenterLat = std::stod(telemetry->lat);
        m_mapCenterLon = std::stod(telemetry->lon);
      }
      catch (const std::exception&) {
      }
    }
    m_pendingMap = mapWorkspaceStatusText("follow-selected",
                                          selectedDrone,
                                          "follow selected drone");
  }

  static std::string
  formatLatLon(double lat, double lon)
  {
    std::ostringstream text;
    text << std::fixed << std::setprecision(5)
         << lat << ", " << lon;
    return text.str();
  }

  struct MapMarker
  {
    std::string id;
    std::string label;
    std::string subtitle;
    bool isDrone = false;
    double lat = 35.1186;
    double lon = -89.9375;
    uint8_t r = 220;
    uint8_t g = 20;
    uint8_t b = 40;
  };

  MapMarker
  markerForDrone(const std::string& droneId, size_t index) const
  {
    const auto found = m_dronePositions.find(droneId);
    double lat = m_groundStationLat;
    double lon = m_groundStationLon + (static_cast<double>(index) + 1.0) * 0.0007;
    if (found != m_dronePositions.end()) {
      lat = found->second.first;
      lon = found->second.second;
    }
    if (std::abs(lat - m_groundStationLat) < 0.00001 &&
        std::abs(lon - m_groundStationLon) < 0.00001) {
      lon += (static_cast<double>(index) + 1.0) * 0.00055;
    }
    MapMarker marker{droneId, std::string("Drone ") + droneId,
                     formatLatLon(lat, lon), true,
                     lat, lon,
                     static_cast<uint8_t>(index == 0 ? 220 : 20),
                     static_cast<uint8_t>(index == 0 ? 40 : 160),
                     static_cast<uint8_t>(index == 0 ? 40 : 70)};
    if (const auto mission = m_runtime.missionForDrone(droneId)) {
      if (mission->isUploading() || mission->isUploaded()) {
        marker.r = 245;
        marker.g = 160;
        marker.b = 20;
      }
      else if (mission->isExecuting()) {
        marker.r = 30;
        marker.g = 160;
        marker.b = 80;
      }
      else if (mission->isStopping()) {
        marker.r = 70;
        marker.g = 110;
        marker.b = 220;
      }
      else if (mission->isCompleted()) {
        marker.r = 20;
        marker.g = 150;
        marker.b = 180;
      }
      else if (mission->isFailed() || mission->isCancelled()) {
        marker.r = 220;
        marker.g = 30;
        marker.b = 40;
      }
    }
    if (const auto progress = m_runtime.missionProgressSnapshot();
        progress && progress->appliesToDrone(droneId)) {
      if (progress->isActive()) {
        marker.r = 130;
        marker.g = 70;
        marker.b = 210;
      }
      else if (progress->isComplete()) {
        marker.r = 20;
        marker.g = 150;
        marker.b = 180;
      }
      else if (progress->isFailed()) {
        marker.r = 220;
        marker.g = 30;
        marker.b = 40;
      }
    }
    if (const auto safety = m_runtime.safetyForDrone(droneId);
        safety && safety->needsOperatorAttention()) {
      marker.r = 220;
      marker.g = 30;
      marker.b = 40;
    }
    marker.subtitle = formatLatLon(lat, lon);
    return marker;
  }

  struct SelectedDroneViewState : SelectedDroneSummaryState
  {
    std::string inspectorText;
    std::string mapText;
    MapMarker marker;
  };

  SelectedDroneViewState
  selectedDroneViewState() const
  {
    SelectedDroneViewState state;
    const auto runtimeView = m_runtime.runtimeSnapshot();
    const auto selectedDrone = runtimeView.selectedDroneId;
    size_t markerIndex = 0;
    const auto foundId = std::find(m_droneIds.begin(), m_droneIds.end(), selectedDrone);
    if (foundId != m_droneIds.end()) {
      markerIndex = static_cast<size_t>(std::distance(m_droneIds.begin(), foundId));
    }
    state.marker = markerForDrone(selectedDrone, markerIndex);
    const auto* drone = runtimeView.findDrone(selectedDrone);
    const std::string notReadyReasons = drone ? drone->notReadyReasonText() : "Ready";
    const std::optional<TelemetryState> telemetry = drone ? drone->telemetry : std::nullopt;
    const std::optional<ReadinessState> readiness = drone ? drone->readiness : std::nullopt;
    const std::optional<MissionState> mission = drone ? drone->mission : std::nullopt;
    const std::optional<VideoState> video = drone ? drone->video : std::nullopt;
    const std::optional<VideoAdaptiveState> videoAdaptive =
      drone ? drone->videoAdaptive : std::nullopt;
    const std::optional<SafetyState> safety = drone ? drone->safety : std::nullopt;
    const auto missionProgress = runtimeView.missionProgress;
    auto missionPlan = runtimeView.missionPlan;
    auto missionPart = m_runtime.missionPartForDrone(selectedDrone);
    const auto command = m_runtime.commandForDrone(selectedDrone);
    std::optional<RuntimeCommandSnapshot> armCommand;
    std::optional<RuntimeCommandSnapshot> landCommand;
    if (drone && drone->commandStates.find("arm") != drone->commandStates.end()) {
      armCommand = drone->commandStates.at("arm");
    }
    if (drone && drone->commandStates.find("land") != drone->commandStates.end()) {
      landCommand = drone->commandStates.at("land");
    }
    if (!missionPlan && m_previewMissionPlan) {
      missionPlan = m_previewMissionPlan;
    }
    if (!missionPart) {
      missionPart = previewMissionPartForDrone(selectedDrone);
    }
    static_cast<SelectedDroneSummaryState&>(state) =
      SelectedDroneSummaryState::fromStates(selectedDrone, telemetry, readiness, mission,
                                            missionPlan, missionPart, missionProgress,
                                            video, videoAdaptive, safety);
    if (telemetry) {
      state.hasTelemetry = true;
      const auto freshness = telemetry->telemetryFreshnessLabel();
      state.inspectorText = telemetry->statusLine() +
        (readiness ? " " + readiness->statusLine() : "") +
        (mission ? " " + mission->statusLine() : "") +
        (missionPlan ? " " + missionPlan->statusLine() : "") +
        (missionPart ? " " + missionPart->statusLine() : "") +
        (missionProgress ? " " + missionProgress->statusLine() : "") +
        (video ? " " + video->statusLine() : "") +
        (videoAdaptive ? " " + videoAdaptive->statusLine() : "") +
        (command ? " " + command->statusLine() : "") +
        (armCommand ? " arm=" + std::string(to_string(armCommand->lifecycle)) +
                      " arm_detail=" + armCommand->detail +
                      " arm_updated_ms=" + std::to_string(armCommand->updatedMs) : "") +
        (landCommand ? " land=" + std::string(to_string(landCommand->lifecycle)) +
                      " land_detail=" + landCommand->detail +
                      " land_updated_ms=" + std::to_string(landCommand->updatedMs) : "") +
        (safety ? " " + safety->statusLine() : "") +
        " telemetry=" + freshness +
        " not-ready: " + notReadyReasons;
      state.mapText = mapTextForTelemetry(*telemetry, mission, state.selectedDrone,
                                          readiness, video, command, safety);
      if (videoAdaptive) {
        state.mapText += "\n" + formatTag("video_stream_health",
                                          videoAdaptive->streamHealthSummary());
        state.mapText += "\n" + formatTag("video_adaptive", videoAdaptive->compactSummary());
        state.mapText += "\n" + formatTag("video_timeout_ms",
                                          std::to_string(videoAdaptive->missingTimeoutMs));
        state.mapText += "\n" + formatTag("video_lookahead",
                                          std::to_string(videoAdaptive->lookahead));
      }
      if (missionProgress) {
        state.mapText += "\n" + formatTag("progress", missionProgress->phase);
        state.mapText += "\n" + formatTag("parts", std::to_string(missionProgress->completedParts) +
                                              "/" +
                                              std::to_string(missionProgress->totalParts));
        state.mapText += "\n" + formatTag("missing_parts",
                                          std::to_string(missionProgress->missingParts));
        state.mapText += "\n" + formatTag("compensated_parts",
                                          std::to_string(missionProgress->compensatedParts));
        state.mapText += "\n" + formatTag("return_home",
                                          std::string(missionProgress->returnHomePlanned ? "yes" : "no"));
      }
      if (missionPart) {
        state.mapText += "\n" + formatTag("part_id", missionPart->id);
        state.mapText += "\n" + formatTag("role", missionPart->role);
        state.mapText += "\n" + formatTag("segment_state", state.missionSegmentState);
        state.mapText += "\n" + formatTag("part_waypoints",
                                          std::to_string(missionPart->waypoints.size()));
        state.mapText += "\n" + formatTag("part_return",
                                          std::string(missionPart->returnHomePlanned ? "yes" : "no"));
      }
    }
    else {
      state.inspectorText = "No telemetry for selected drone " + state.selectedDrone;
      state.inspectorText += " telemetry=missing not-ready: " + notReadyReasons;
      state.mapText = mapWorkspaceStatusText("waiting-for-telemetry",
                                             state.selectedDrone,
                                             "click-map-to-plan-mission");
      state.mapText = formatTag("telemetry_freshness", "missing") + "\n" + state.mapText;
      if (missionProgress) {
        state.inspectorText += " " + missionProgress->statusLine();
        state.mapText += "\n" + formatTag("progress", missionProgress->phase);
        state.mapText += "\n" + formatTag("parts",
                                         std::to_string(missionProgress->completedParts) + "/" +
                                         std::to_string(missionProgress->totalParts));
        state.mapText += "\n" + formatTag("missing_parts",
                                         std::to_string(missionProgress->missingParts));
        state.mapText += "\n" + formatTag("compensated_parts",
                                         std::to_string(missionProgress->compensatedParts));
        state.mapText += "\n" + formatTag("return_home",
                                          std::string(missionProgress->returnHomePlanned ? "yes" : "no"));
      }
      if (missionPlan) {
        state.inspectorText += " " + missionPlan->statusLine();
      }
      if (missionPart) {
        state.inspectorText += " " + missionPart->statusLine();
        state.mapText += "\n" + formatTag("part_id", missionPart->id);
        state.mapText += "\n" + formatTag("role", missionPart->role);
        state.mapText += "\n" + formatTag("segment_state", state.missionSegmentState);
        state.mapText += "\n" + formatTag("part_waypoints", std::to_string(missionPart->waypoints.size()));
        state.mapText += "\n" + formatTag("part_return",
                                         std::string(missionPart->returnHomePlanned ? "yes" : "no"));
      }
      if (videoAdaptive) {
        state.inspectorText += " " + videoAdaptive->streamHealthSummary() +
                               " " + videoAdaptive->statusLine();
        state.mapText += "\n" + formatTag("video_stream_health",
                                          videoAdaptive->streamHealthSummary());
        state.mapText += "\n" + formatTag("video_adaptive", videoAdaptive->compactSummary());
        state.mapText += "\n" + formatTag("video_timeout_ms",
                                          std::to_string(videoAdaptive->missingTimeoutMs));
        state.mapText += "\n" + formatTag("video_lookahead",
                                          std::to_string(videoAdaptive->lookahead));
      }
    }
    return state;
  }

  void
  logSelectedDroneViewState(const std::string& phase) const
  {
    const auto state = selectedDroneViewState();
    std::ostringstream os;
    os << "SELECTED_VIEW_STATE phase=" << phase
       << " selected=" << state.selectedDrone
       << " has_telemetry=" << (state.hasTelemetry ? "true" : "false")
       << " readiness=" << state.readiness
       << " mission=" << state.missionPhase
       << " mission_progress=" << state.missionProgressPhase
       << " mission_segment_state=" << state.missionSegmentState
       << " mission_plan=" << state.missionPlanTask
       << " mission_part=" << state.missionPartId
       << " mission_part_waypoints=" << state.missionPartWaypoints
       << " video=" << state.videoStatus
       << " video_adaptive=" << state.videoAdaptive
       << " link=" << state.linkState
       << " safety_attention=" << (state.safetyAttention ? "true" : "false")
       << " can_arm=" << (state.canArm ? "true" : "false")
       << " arm_reason=" << state.armReason
       << " can_takeoff=" << (state.canTakeoff ? "true" : "false")
       << " takeoff_reason=" << state.takeoffReason
       << " can_land=" << (state.canLand ? "true" : "false")
       << " land_reason=" << state.landReason
       << " can_manual=" << (state.canManualControl ? "true" : "false")
       << " manual_reason=" << state.manualControlReason
       << " can_panel=" << (state.canControlPanel ? "true" : "false")
       << " panel_reason=" << state.controlPanelReason
       << " marker=" << state.marker.label;
    NDN_LOG_INFO(os.str());
  }

  void
  logVideoAdaptiveViewState(const std::string& phase) const
  {
    const auto droneId = m_runtime.targetDroneId();
    const auto adaptive = m_runtime.videoAdaptiveForDrone(droneId);
    std::ostringstream os;
    os << "VIDEO_ADAPTIVE_VIEW_STATE phase=" << phase
       << " selected=" << droneId
       << " has_adaptive=" << (adaptive ? "true" : "false");
    if (adaptive) {
      os << " state=" << adaptive->state
         << " rtt_ms=" << adaptive->rttMs
         << " requested_bitrate_kbps=" << adaptive->requestedBitrateKbps
         << " accepted_bitrate_kbps=" << adaptive->acceptedBitrateKbps
         << " suggested_bitrate_kbps=" << adaptive->suggestedBitrateKbps
         << " bitrate_action=" << adaptive->bitrateAction
         << " bitrate_reason=" << adaptive->bitrateReason
         << " window=" << adaptive->window
         << " lookahead=" << adaptive->lookahead
         << " future_probe_limit=" << adaptive->futureProbeLimit
         << " interest_lifetime_ms=" << adaptive->interestLifetimeMs
         << " missing_timeout_ms=" << adaptive->missingTimeoutMs
         << " pressure=" << adaptive->maxPressure()
         << " primary_pressure=" << adaptive->primaryPressure
         << " policy_reason=" << adaptive->policyReason
         << " pending_chunks=" << adaptive->pendingChunks
         << " received_chunks=" << adaptive->receivedChunks
         << " decoded_frames=" << adaptive->decodedFrames;
    }
    NDN_LOG_INFO(os.str());
  }

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
                const std::string& label, const std::string& subtitle,
                bool isDrone, uint8_t r, uint8_t g, uint8_t b)
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
    if (isDrone) {
      for (int d = -4; d <= 4; ++d) {
        setPixel(cx + d, cy + d, r, g, b);
        setPixel(cx + d, cy - d, r, g, b);
      }
      setPixel(cx - 5, cy + 2, r, g, b);
      setPixel(cx + 5, cy + 2, r, g, b);
      setPixel(cx - 4, cy + 1, r, g, b);
      setPixel(cx - 3, cy + 0, r, g, b);
      setPixel(cx - 2, cy + 1, r, g, b);
      setPixel(cx - 1, cy + 2, r, g, b);
      setPixel(cx, cy + 2, 255, 255, 255);
      setPixel(cx + 1, cy + 2, r, g, b);
      setPixel(cx + 2, cy + 1, r, g, b);
      setPixel(cx + 3, cy + 0, r, g, b);
      setPixel(cx + 4, cy + 1, r, g, b);
    }
    else {
      for (int d = -4; d <= 4; ++d) {
        setPixel(cx + d, cy + d, r, g, b);
        setPixel(cx + d, cy - d, r, g, b);
      }
    }
    const int textX = std::clamp(cx + 10, 0, width - 48);
    drawTinyText(pixbuf, textX, std::clamp(cy - 4, 0, height - 8), label, r, g, b);
    if (!subtitle.empty()) {
      drawTinyText(pixbuf, textX, std::clamp(cy + 4, 0, height - 16),
                   subtitle, r, g, b);
    }
  }

  std::vector<MapMarker>
  currentMapMarkers() const
  {
    std::vector<MapMarker> markers;
    markers.push_back({"GS", "GS", formatLatLon(m_groundStationLat, m_groundStationLon), false,
                       m_groundStationLat, m_groundStationLon, 20, 80, 220});
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      markers.push_back(markerForDrone(m_droneIds[i], i));
    }
    if (!m_planWaypoints.empty()) {
      for (size_t i = 0; i < m_planWaypoints.size(); ++i) {
        markers.push_back({"WP" + std::to_string(i + 1),
                           "WP" + std::to_string(i + 1),
                           formatLatLon(m_planWaypoints[i].first, m_planWaypoints[i].second),
                           false,
                           m_planWaypoints[i].first, m_planWaypoints[i].second,
                           245, 160, 20});
      }
    }
    else if (m_hasPatrolCenter) {
      markers.push_back({"WP", "WP", formatLatLon(m_patrolCenterLat, m_patrolCenterLon), false,
                        m_patrolCenterLat, m_patrolCenterLon, 245, 160, 20});
    }
    if (m_previewMissionPlan) {
      for (size_t partIndex = 0; partIndex < m_previewMissionPlan->parts.size(); ++partIndex) {
        const auto& part = m_previewMissionPlan->parts[partIndex];
        const uint8_t r = static_cast<uint8_t>(partIndex == 0 ? 120 : 35);
        const uint8_t g = static_cast<uint8_t>(partIndex == 0 ? 80 : 150);
        const uint8_t b = static_cast<uint8_t>(partIndex == 0 ? 230 : 95);
        const auto prefix = part.assignedDrone.empty() ? ("P" + std::to_string(partIndex + 1)) :
                            part.assignedDrone;
        for (size_t waypointIndex = 0; waypointIndex < part.waypoints.size(); ++waypointIndex) {
          const auto& waypoint = part.waypoints[waypointIndex];
          const auto isReturnHome = part.returnHomePlanned &&
                                    waypointIndex + 1 == part.waypoints.size();
          markers.push_back({prefix + (isReturnHome ? "R" : std::to_string(waypointIndex + 1)),
                            prefix + (isReturnHome ? "R" : std::to_string(waypointIndex + 1)),
                            formatLatLon(waypoint.lat, waypoint.lon), false,
                            waypoint.lat, waypoint.lon, r, g, b});
        }
      }
    }
    return markers;
  }

  std::optional<MissionPart>
  previewMissionPartForDrone(const std::string& droneId) const
  {
    if (!m_previewMissionPlan) {
      return std::nullopt;
    }
    for (const auto& part : m_previewMissionPlan->parts) {
      if (part.assignedDrone == droneId) {
        return part;
      }
    }
    return std::nullopt;
  }

  double
  patrolSideMeters() const
  {
    try {
      return std::stod(m_patrolSizeMeters.get_text());
    }
    catch (const std::exception&) {
      return 140.0;
    }
  }

  MissionPlan
  buildMissionPlanPreview() const
  {
    std::vector<MissionWaypoint> route;
    route.reserve(m_planWaypoints.size());
    for (const auto& point : m_planWaypoints) {
      route.push_back({point.first, point.second});
    }

    std::map<std::string, MissionWaypoint> departures;
    for (const auto& droneId : m_droneIds) {
      const auto found = m_dronePositions.find(droneId);
      if (found != m_dronePositions.end()) {
        departures.emplace(droneId, MissionWaypoint{found->second.first, found->second.second});
      }
    }

    return buildPatrolMissionPlan("preview-current", m_patrolCenterLat, m_patrolCenterLon,
                                  patrolSideMeters(), m_droneIds, route, departures);
  }

  void
  refreshMissionPlanPreview(const std::string& phase)
  {
    if (m_planWaypoints.empty()) {
      m_previewMissionPlan.reset();
      NDN_LOG_INFO("MISSION_PREVIEW phase=" << phase << " cleared=true");
      return;
    }
    m_previewMissionPlan = buildMissionPlanPreview();
    NDN_LOG_INFO("MISSION_PREVIEW phase=" << phase << " " << m_previewMissionPlan->statusLine());
    for (const auto& part : m_previewMissionPlan->parts) {
      NDN_LOG_INFO("MISSION_PREVIEW_PART phase=" << phase << " " << part.statusLine() <<
                   " waypoints=" << part.waypointText());
    }
  }

  void
  updatePatrolInputsFromWaypoints()
  {
    if (m_planWaypoints.empty()) {
      m_hasPatrolCenter = false;
      m_patrolLat.set_text("");
      m_patrolLon.set_text("");
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
    refreshMissionPlanPreview("undo-waypoint");
    m_mapMission.set_text(mapWorkspaceStatusText("removed-last-waypoint",
                                                 m_runtime.targetDroneId(),
                                                 "review-or-upload-mission"));
    refreshMapTile();
  }

  void
  clearMissionWaypoints()
  {
    m_planWaypoints.clear();
    updatePatrolInputsFromWaypoints();
    m_previewMissionPlan.reset();
    refreshMissionPlanPreview("clear-waypoints");
    m_mapMission.set_text(mapWorkspaceStatusText("waypoints-cleared",
                                                 m_runtime.targetDroneId(),
                                                 "click-map-to-add-waypoint"));
    refreshMapTile();
  }

  std::optional<MissionPlan>
  currentEditableMissionPlan() const
  {
    if (m_previewMissionPlan) {
      return m_previewMissionPlan;
    }
    if (!m_planWaypoints.empty()) {
      return buildMissionPlanPreview();
    }
    return m_runtime.missionPlanSnapshot();
  }

  static std::vector<std::pair<double, double>>
  routeWaypointsFromPlan(const MissionPlan& plan)
  {
    std::vector<std::pair<double, double>> route;
    for (const auto& part : plan.parts) {
      const auto limit = part.returnHomePlanned && !part.waypoints.empty() ?
                         part.waypoints.size() - 1 : part.waypoints.size();
      for (size_t i = 0; i < limit; ++i) {
        route.emplace_back(part.waypoints[i].lat, part.waypoints[i].lon);
      }
      if (!route.empty()) {
        break;
      }
    }
    return route;
  }

  void
  saveMissionPlanFile()
  {
    const auto path = m_missionPlanFile.get_text();
    const auto plan = currentEditableMissionPlan();
    if (!plan) {
      m_status.set_text("No mission plan preview or uploaded plan to save");
      return;
    }
    std::string detail;
    if (m_runtime.saveMissionPlanToFile(*plan, path, &detail)) {
      m_runtime.setMissionPlanFilePath(path);
      m_status.set_text("Mission plan saved: " + detail);
      m_mapMission.set_text(mapWorkspaceStatusText("mission-plan-saved",
                                                   m_runtime.targetDroneId(),
                                                   path));
    }
    else {
      m_status.set_text("Mission plan save failed: " + detail);
    }
  }

  void
  loadMissionPlanFile()
  {
    const auto path = m_missionPlanFile.get_text();
    std::string detail;
    if (!m_runtime.loadMissionPlanFromFile(path, &detail)) {
      m_status.set_text("Mission plan load failed: " + detail);
      return;
    }
    m_previewMissionPlan = m_runtime.missionPlanSnapshot();
    if (m_previewMissionPlan) {
      m_planWaypoints = routeWaypointsFromPlan(*m_previewMissionPlan);
      updatePatrolInputsFromWaypoints();
    }
    updateVehicleRows();
    m_mapMission.set_text(mapWorkspaceStatusText("mission-plan-loaded",
                                                 m_runtime.targetDroneId(),
                                                 path));
    m_status.set_text("Mission plan loaded: " + detail);
    refreshMapTile();
  }

  void
  scheduleMissionStartPhase(int phase, size_t droneIndex)
  {
    const auto startableDrones = m_runtime.missionStartableDrones();
    const auto eligibleDrones = missionStartEligibleDrones();
    if (startableDrones.empty()) {
      m_missionStartInFlight = false;
      m_status.set_text("No uploaded patrol mission is ready; upload mission before Start Mission");
      updateSelectedActionControls();
      return;
    }
    if (eligibleDrones.empty()) {
      const auto state = missionControlState();
      m_missionStartInFlight = false;
      m_status.set_text("Patrol mission start blocked: " + state.startBlocked);
      updateSelectedActionControls();
      return;
    }
    if (droneIndex >= eligibleDrones.size()) {
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
      m_missionStartInFlight = false;
      updateVehicleRows();
      return;
    }

    const auto droneId = eligibleDrones[droneIndex];
    if (phase == 0) {
      const auto flightGate = flightSafetyGateForDrone(droneId);
      if (!flightGate || !flightGate->canArm) {
        m_status.set_text("Mission sequence: Drone " + droneId +
                          " arm skipped (" +
                          (flightGate ? flightGate->armReason : std::string("no-flight-gate")) + ")");
        Glib::signal_timeout().connect([this, droneIndex] {
          scheduleMissionStartPhase(0, droneIndex + 1);
          return false;
        }, 250);
        return;
      }
      m_status.set_text("Mission sequence: arming Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(eligibleDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "arm", {{"arm", "true"}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(0, droneIndex + 1);
        return false;
      }, 900);
      return;
    }
    if (phase == 1) {
      const auto flightGate = flightSafetyGateForDrone(droneId);
      if (!flightGate || !flightGate->canTakeoff) {
        m_status.set_text("Mission sequence: Drone " + droneId +
                          " takeoff blocked (" +
                          (flightGate ? flightGate->takeoffReason : std::string("no-flight-gate")) + ")");
        Glib::signal_timeout().connect([this, droneIndex] {
          scheduleMissionStartPhase(1, droneIndex + 1);
          return false;
        }, 250);
        return;
      }
      m_status.set_text("Mission sequence: takeoff Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(eligibleDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(1, droneIndex + 1);
        return false;
      }, 900);
      return;
    }

    m_status.set_text("Mission sequence: starting mission on Drone " + droneId +
                      " (" + std::to_string(droneIndex + 1) + "/" +
                      std::to_string(eligibleDrones.size()) + ")");
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
      m_patrolStopInFlight = false;
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
    const auto imageX = std::clamp(pixelX, 0.0, static_cast<double>(width - 1));
    const auto imageY = std::clamp(pixelY, 0.0, static_cast<double>(height - 1));
    return {
      imageX * 256.0 / static_cast<double>(width),
      imageY * 256.0 / static_cast<double>(height)
    };
  }

  void
  beginMapPointer(double pixelX, double pixelY)
  {
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    m_mapPointerDown = true;
    m_mapPointerStartX = imagePixel.first;
    m_mapPointerStartY = imagePixel.second;
  }

  void
  finishMapPointer(double pixelX, double pixelY)
  {
    if (!m_mapPointerDown) {
      return;
    }
    m_mapPointerDown = false;
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    const auto dx = imagePixel.first - m_mapPointerStartX;
    const auto dy = imagePixel.second - m_mapPointerStartY;
    if (dx * dx + dy * dy < 36.0) {
      placePatrolCenterFromMapClick(imagePixel.first, imagePixel.second);
      return;
    }
    m_mapMission.set_text(mapWorkspaceStatusText("map-click-drag-ignored",
                                                 m_runtime.targetDroneId(),
                                                 "use-pan-mode-to-drag"));
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
      m_mapMission.set_text(mapWorkspaceStatusText("pan-cancelled",
                                                   m_runtime.targetDroneId(),
                                                   "drag-move-before-release"));
      return;
    }
    m_mapMission.set_text(mapWorkspaceStatusText("map-panned",
                                                 m_runtime.targetDroneId(),
                                                 "click-map-or-center-gs"));
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
  zoomMapAt(int delta, double pixelX, double pixelY)
  {
    const int oldZoom = m_mapZoom;
    const int newZoom = std::clamp(oldZoom + delta, 2, 19);
    if (newZoom == oldZoom) {
      return;
    }

    const auto clampedPixel = mapEventToImagePixel(pixelX, pixelY);
    const auto oldTileX = (m_mapSourcePixelX + clampedPixel.first) / 256.0;
    const auto oldTileY = (m_mapSourcePixelY + clampedPixel.second) / 256.0;
    const auto [cursorLat, cursorLon] = latLonForTileFloat(oldTileX, oldTileY, oldZoom);

    m_mapZoom = newZoom;
    const auto [cursorTileX, cursorTileY] = tileFloatForLatLon(cursorLat, cursorLon, m_mapZoom);
    const auto newCenterTileX = cursorTileX - clampedPixel.first / 256.0 + 0.5;
    const auto newCenterTileY = cursorTileY - clampedPixel.second / 256.0 + 0.5;
    const auto [newCenterLat, newCenterLon] =
      latLonForTileFloat(newCenterTileX, newCenterTileY, m_mapZoom);
    m_mapCenterLat = newCenterLat;
    m_mapCenterLon = newCenterLon;
    refreshMapTile();
  }

  void
  centerMapOnGroundStation()
  {
    m_mapCenterLat = m_groundStationLat;
    m_mapCenterLon = m_groundStationLon;
    m_mapMission.set_text(mapWorkspaceStatusText("centered-on-gs",
                                                 m_runtime.targetDroneId(),
                                                 "click-map-to-add-waypoint"));
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
    refreshMissionPlanPreview("waypoint-added");
    std::ostringstream pointText;
    pointText << std::fixed << std::setprecision(6)
              << latLon.first << "," << latLon.second;
    m_mapMission.set_text(mapWorkspaceStatusText("added-wp" +
                                                 std::to_string(m_planWaypoints.size()) +
                                                 " " + pointText.str(),
                                                 m_runtime.targetDroneId(),
                                                 "upload-mission-or-edit-wps"));
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
              drawMapMarker(marked, markerX, markerY, marker.label, marker.subtitle,
                           marker.isDrone, marker.r, marker.g, marker.b);
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
  Gtk::ScrolledWindow m_scroll;
  Gtk::Box m_box;
  Gtk::FlowBox m_buttons;
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
  Gtk::ScrolledWindow m_statusScroll;
  Gtk::Box m_statusPanel;
  Gtk::Frame m_dashboardInspectorFrame;
  Gtk::Frame m_flightInspectorFrame;
  Gtk::Frame m_telemetryInspectorFrame;
  Gtk::Frame m_cameraInspectorFrame;
  Gtk::Frame m_videoInspectorFrame;
  Gtk::Frame m_missionInspectorFrame;
  Gtk::Frame m_missionDetailFrame;
  Gtk::Frame m_authorityInspectorFrame;
  Gtk::Frame m_catalogInspectorFrame;
  Gtk::Frame m_parameterInspectorFrame;
  Gtk::Frame m_functionalityInspectorFrame;
  Gtk::Frame m_practicalityInspectorFrame;
  Gtk::Frame m_stabilityInspectorFrame;
  Gtk::Frame m_commandHistoryFrame;
  Gtk::Frame m_serviceInspectorFrame;
  Gtk::Frame m_eventInspectorFrame;
  Gtk::Button m_start;
  Gtk::Button m_stop;
  Gtk::Button m_applyBitrate;
  Gtk::Button m_arm;
  Gtk::Button m_takeoff;
  Gtk::Button m_land;
  Gtk::Button m_emergencyStop;
  Gtk::Button m_patrol;
  Gtk::Button m_startMission;
  Gtk::Button m_stopPatrol;
  Gtk::Button m_saveMissionPlan;
  Gtk::Button m_loadMissionPlan;
  Gtk::Button m_controlToggle;
  Gtk::Button m_refreshRecording;
  Gtk::Button m_browseRepo;
  Gtk::Button m_fetchParameters;
  Gtk::Button m_refreshAuthority;
  Gtk::Button m_playRecording;
  Gtk::Button m_mapZoomIn;
  Gtk::Button m_mapZoomOut;
  Gtk::ToggleButton m_mapPanMode;
  Gtk::ToggleButton m_mapFollowSelected;
  Gtk::Button m_mapCenterGs;
  Gtk::Button m_mapUndoWp;
  Gtk::Button m_mapClearWp;
  Gtk::Label m_patrolHint;
  Gtk::Entry m_patrolLat;
  Gtk::Entry m_patrolLon;
  Gtk::Entry m_patrolSizeMeters;
  Gtk::Entry m_missionPlanFile;
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
  Gtk::Label m_dashboardInspector;
  Gtk::Label m_flightInspector;
  Gtk::Label m_cameraInspector;
  Gtk::Label m_videoInspector;
  Gtk::Label m_commandHistory;
  Gtk::Label m_telemetryInspector;
  Gtk::Label m_missionInspector;
  Gtk::Label m_missionDetail;
  Gtk::Label m_authorityInspector;
  Gtk::Label m_catalogInspector;
  Gtk::Label m_parameterInspector;
  Gtk::Label m_functionalityInspector;
  Gtk::Label m_practicalityInspector;
  Gtk::Label m_stabilityInspector;
  Gtk::Image m_image;
  Gtk::Label m_stats;
  Glib::Dispatcher m_statusDispatcher;
  Glib::Dispatcher m_frameDispatcher;
  Glib::Dispatcher m_gamepadDispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
  std::string m_pendingLinkStatus;
  std::string m_pendingTelemetry;
  std::string m_lastInspectorDetail;
  std::string m_pendingMap;
  bool m_pendingMapRefresh = false;
  std::string m_pendingMapLat;
  std::string m_pendingMapLon;
  bool m_pendingVehicleRowsRefresh = false;
  std::string m_mapTileKey;
  std::atomic<bool> m_mapTileLoading{false};
  std::atomic<bool> m_mapRefreshPending{false};
  std::atomic<uint64_t> m_mapRenderGeneration{0};
  const double m_groundStationLat;
  const double m_groundStationLon;
  double m_mapCenterLat;
  double m_mapCenterLon;
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
  bool m_mapPointerDown = false;
  double m_mapPointerStartX = 0.0;
  double m_mapPointerStartY = 0.0;
  bool m_hasPatrolCenter = false;
  double m_patrolCenterLat = 35.1186;
  double m_patrolCenterLon = -89.9375;
  std::vector<std::pair<double, double>> m_planWaypoints;
  std::optional<MissionPlan> m_previewMissionPlan;
  std::map<std::string, std::pair<double, double>> m_dronePositions;
  size_t m_telemetryPollIndex = 0;
  bool m_autoApplyBitrateTest = false;
  bool m_autoVideoPressureProfileTest = false;
  bool m_autoRepeatStopTest = false;
  bool m_autoDashboardPanelTest = false;
  uint64_t m_operatorAuthorityRefreshIntervalMs = 0;
  Glib::RefPtr<Gdk::Pixbuf> m_pendingPixbuf;
  uint64_t m_pendingSeq = 0;
  uint64_t m_pendingElapsedMs = 0;
  uint64_t m_pendingFrameGeneration = 0;
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
  std::atomic<bool> m_patrolUploadInFlight{false};
  std::atomic<bool> m_missionStartInFlight{false};
  std::atomic<bool> m_patrolStopInFlight{false};
  std::atomic<bool> m_authorityRefreshInFlight{false};
  std::atomic<bool> m_useGamepad{false};
  std::atomic<bool> m_manualActive{false};
  std::atomic<int> m_manualX{0};
  std::atomic<int> m_manualY{0};
  std::atomic<int> m_manualZ{500};
  std::atomic<int> m_manualR{0};
  std::array<std::atomic<int>, 8> m_gamepadAxes{};
  std::array<std::atomic<bool>, 16> m_gamepadButtons{};
  uint64_t m_streamGeneration = 0;
  uint64_t m_lastDisplayedSeq = 0;
  uint64_t m_lastDisplayedGeneration = 0;
  bool m_acceptFrames = false;
  std::atomic<uint64_t> m_decodedFrames{0};
  std::vector<std::string> m_droneIds;
};
