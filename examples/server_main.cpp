#include <iostream>
#include <sstream>  // std::ostringstream
#include <string>

#include "examples/messages.pb.h"
#include <gtkmm.h>

class ExampleWindow : public Gtk::Window
{

public:

    ExampleWindow()
    : m_btn("Show Image"),
    m_image("/home/tianxing/NDN/drone-car.png")
    {
        add(m_btn);
        m_btn.signal_clicked().connect([this](){OnButtonClicked();});
    }

private:

    void OnButtonClicked()
    {
        m_window = std::make_unique<Gtk::Window>();
        m_window->add(m_image);
        m_window->show_all();

    }

    Gtk::Button m_btn;
    Gtk::Image m_image;
    std::unique_ptr<Gtk::Window> m_window;
};

int
main(int argc, char **argv)
{
    // muas::ObjectDetection_YOLOv8_Request _request;
    // _request.set_image_str("image_str");
    // std::string buffer="";
    // _request.SerializeToString(&buffer);
    // std::cout << "Serialized to String: " << buffer;
    // muas::ObjectDetection_YOLOv8_Request _request2;
    // _request2.ParseFromString(buffer);
    // std::cout << "image_str String: " << _request.image_str();

    auto app = Gtk::Application::create(argc, argv, "so.question.q65011763");
    
    ExampleWindow window;
    window.show_all();

    return app->run(window);

}