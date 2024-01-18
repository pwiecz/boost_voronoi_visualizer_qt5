// Boost.Polygon library voronoi_visualizer.cpp file

//          Copyright Andrii Sydorchuk 2010-2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org for updates, documentation, and revision history.

#include <array>
#include <iostream>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPushButton>
#include <QTextStream>

#include <boost/polygon/polygon.hpp>
#include <boost/polygon/voronoi.hpp>
using namespace boost::polygon;

#include "voronoi_visual_utils.hpp"


static const char* vertex_shader_code = R"(#ifdef GL_ES
precision mediump float;
#endif
attribute vec2 position;
uniform mat4 mvpMatrix;
void main(void) {
    gl_Position = mvpMatrix * vec4(position, 0.0, 1.0);
}
)";

static const char* fragment_shader_code = R"(#ifdef GL_ES
precision mediump float;
#endif
uniform vec4 color;
void main(void) {
  gl_FragColor = color;
}
)";

class GLWidget : public QOpenGLWidget, public QOpenGLFunctions {
  Q_OBJECT

 public:
  explicit GLWidget(QWidget* parent = NULL) :
      QOpenGLWidget(parent),
      primary_edges_only_(false),
      internal_edges_only_(false) {
    setUpdateBehavior(QOpenGLWidget::UpdateBehavior::NoPartialUpdate);
    startTimer(40);
  }

  QSize sizeHint() const {
    return QSize(600, 600);
  }

  void build(const QString& file_path) {
    // Clear all containers.
    clear();

    // Read data.
    read_data(file_path);

    // No data, don't proceed.
    if (!brect_initialized_) {
      return;
    }

    // Construct bounding rectangle.
    construct_brect();

    // Construct voronoi diagram.
    construct_voronoi(
        point_data_.begin(), point_data_.end(),
        segment_data_.begin(), segment_data_.end(),
        &vd_);

    // Color exterior edges.
    for (const_edge_iterator it = vd_.edges().begin();
         it != vd_.edges().end(); ++it) {
      if (!it->is_finite()) {
        color_exterior(&(*it));
      }
    }

    // Update view port.
    update_view_port();
  }

  void show_primary_edges_only() {
    primary_edges_only_ ^= true;
  }

  void show_internal_edges_only() {
    internal_edges_only_ ^= true;
  }

 protected:
  void initializeGL() {
    initializeOpenGLFunctions();
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glEnable(GL_POINT_SMOOTH);
    vertex_shader_ = prepare_shader(GL_VERTEX_SHADER, vertex_shader_code);
    fragment_shader_ = prepare_shader(GL_FRAGMENT_SHADER, fragment_shader_code);
    gl_program_ = glCreateProgram();
    glAttachShader(gl_program_, vertex_shader_);
    glAttachShader(gl_program_, fragment_shader_);
    glLinkProgram(gl_program_);
    glDeleteShader(vertex_shader_);
    glDeleteShader(fragment_shader_);
    mvp_matrix_location_ = glGetUniformLocation(gl_program_, "mvpMatrix");
    assert(mvp_matrix_location_ >= 0);
    vertex_location_ = glGetAttribLocation(gl_program_, "position");
    assert(vertex_location_ >= 0);
    color_location_ = glGetUniformLocation(gl_program_, "color");
    assert(color_location_ >= 0);
  }

  GLuint prepare_shader(GLenum type, const char* shader_code) {
      GLuint shaderId = glCreateShader(type);
      GLint length = (GLint)std::strlen(shader_code);
      glShaderSource(shaderId, 1, &shader_code, &length);
      glCompileShader(shaderId);
      GLint result;
      glGetShaderiv(shaderId, GL_COMPILE_STATUS, &result);
      if (result == (GLint)GL_FALSE) {
          GLint logSize = 0;
          glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &logSize);

          std::vector<GLchar> errorLog(logSize);
          glGetShaderInfoLog(shaderId, logSize, &logSize, errorLog.data());

          glDeleteShader(shaderId);
          return 0;
      }
      return shaderId;

  }

  void paintGL() {
    glClearColor(1.f, 1.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_points();
    draw_segments();
    draw_vertices();
    draw_edges();
  }

  void resizeGL(int width, int height) {
    int side = qMin(width, height);
    glViewport((width - side) / 2, (height - side) / 2, side, side);
  }

  void timerEvent(QTimerEvent* e) {
    update();
  }

 private:
  typedef double coordinate_type;
  typedef point_data<coordinate_type> point_type;
  typedef segment_data<coordinate_type> segment_type;
  typedef rectangle_data<coordinate_type> rect_type;
  typedef voronoi_builder<int> VB;
  typedef voronoi_diagram<coordinate_type> VD;
  typedef VD::cell_type cell_type;
  typedef VD::cell_type::source_index_type source_index_type;
  typedef VD::cell_type::source_category_type source_category_type;
  typedef VD::edge_type edge_type;
  typedef VD::cell_container_type cell_container_type;
  typedef VD::cell_container_type vertex_container_type;
  typedef VD::edge_container_type edge_container_type;
  typedef VD::const_cell_iterator const_cell_iterator;
  typedef VD::const_vertex_iterator const_vertex_iterator;
  typedef VD::const_edge_iterator const_edge_iterator;

  static const std::size_t EXTERNAL_COLOR = 1;

  void clear() {
    brect_initialized_ = false;
    point_data_.clear();
    segment_data_.clear();
    vd_.clear();

    clear_vbo_array(gl_points_);
    clear_vbo(gl_segments_);
    clear_vbo_array(gl_vertices_);
    clear_vbo_array(gl_edges_);
  }

  void read_data(const QString& file_path) {
    QFile data(file_path);
    if (!data.open(QFile::ReadOnly)) {
      QMessageBox::warning(
          this, tr("Voronoi Visualizer"),
          tr("Disable to open file ") + file_path);
    }
    QTextStream in_stream(&data);
    std::size_t num_points, num_segments;
    int x1, y1, x2, y2;
    in_stream >> num_points;
    for (std::size_t i = 0; i < num_points; ++i) {
      in_stream >> x1 >> y1;
      point_type p(x1, y1);
      update_brect(p);
      point_data_.push_back(p);
    }
    in_stream >> num_segments;
    for (std::size_t i = 0; i < num_segments; ++i) {
      in_stream >> x1 >> y1 >> x2 >> y2;
      point_type lp(x1, y1);
      point_type hp(x2, y2);
      update_brect(lp);
      update_brect(hp);
      segment_data_.push_back(segment_type(lp, hp));
    }
    in_stream.flush();
  }

  void update_brect(const point_type& point) {
    if (brect_initialized_) {
      encompass(brect_, point);
    } else {
      set_points(brect_, point, point);
      brect_initialized_ = true;
    }
  }

  void construct_brect() {
    double side = (std::max)(xh(brect_) - xl(brect_), yh(brect_) - yl(brect_));
    center(shift_, brect_);
    set_points(brect_, shift_, shift_);
    bloat(brect_, side * 1.2);
  }

  void color_exterior(const VD::edge_type* edge) {
    if (edge->color() == EXTERNAL_COLOR) {
      return;
    }
    edge->color(EXTERNAL_COLOR);
    edge->twin()->color(EXTERNAL_COLOR);
    const VD::vertex_type* v = edge->vertex1();
    if (v == NULL || !edge->is_primary()) {
      return;
    }
    v->color(EXTERNAL_COLOR);
    const VD::edge_type* e = v->incident_edge();
    do {
      color_exterior(e);
      e = e->rot_next();
    } while (e != v->incident_edge());
  }

  void update_view_port() {
    rect_type view_rect = brect_;
    deconvolve(view_rect, shift_);
    const float width = xh(view_rect) - xl(view_rect);
    const float height = yh(view_rect) - yl(view_rect);
    projection_matrix_[0] = 2.f / width;
    projection_matrix_[5] = 2.f / height;
    projection_matrix_[10] = -1.f;
    projection_matrix_[15] = 1.f;
    projection_matrix_[12] = -(xl(view_rect) + xh(view_rect)) / width;
    projection_matrix_[13] = -(yl(view_rect) + yh(view_rect)) / height;
    projection_matrix_[14] = 0.f;
  }

  struct GLPoint {
    GLPoint(float x, float y)
      : x_(x), y_(y) {}
    float x_;
    float y_;
  };
  struct VBO {
    VBO(GLuint id, size_t vertex_count)
      : id_(id), vertex_count_(vertex_count) {}
    GLuint id_;
    size_t vertex_count_;
  };

  VBO point_vbo(const point_type& point, float radius_px) {
      const float width = xh(brect_) - xl(brect_);
      const float height = yh(brect_) - yl(brect_);
      const float xRadius = radius_px * width / size().width();
      const float yRadius = radius_px * height / size().height();
      static constexpr size_t boundary_point_count = 20;
      static constexpr float angle_increment = 2 * 3.14159265358979323846 / boundary_point_count;
      std::vector<GLPoint> boundary;
      boundary.reserve(boundary_point_count + 2);
      boundary.emplace_back(point.x(), point.y());
      for (int i = 0; i <= boundary_point_count; ++i) {
          const float angle = angle_increment * i;
          boundary.emplace_back(point.x() + std::sin(angle) * xRadius, point.y() + std::cos(angle) * yRadius);
      }
      GLuint buffer_id;
      glGenBuffers(1, &buffer_id);
      glBindBuffer(GL_ARRAY_BUFFER, buffer_id);
      glBufferData(GL_ARRAY_BUFFER, boundary.size() * sizeof(GLPoint), boundary.data(), GL_STATIC_DRAW);
      return VBO(buffer_id, boundary.size());
  }

  void clear_vbo(VBO& vbo) {
      glDeleteBuffers(1, &vbo.id_);
      vbo.id_ = 0;
  }

  void clear_vbo_array(std::vector<VBO>& vbos) {
      for (VBO& vbo : vbos) {
          clear_vbo(vbo);
      }
      vbos.clear();
  }

  void prepare_points() {
      static constexpr float radius = 4.5f;
      if (!gl_points_.empty()) {
          return;
      }
      gl_points_.reserve(point_data_.size() + 2 * segment_data_.size());
      for (std::size_t i = 0; i < point_data_.size(); ++i) {
          point_type point = point_data_[i];
          point = deconvolve(point, shift_);
          gl_points_.push_back(point_vbo(point, radius));
      }
      for (std::size_t i = 0; i < segment_data_.size(); ++i) {
          point_type lp = low(segment_data_[i]);
          lp = deconvolve(lp, shift_);
          gl_points_.push_back(point_vbo(lp, radius));
          point_type hp = high(segment_data_[i]);
          hp = deconvolve(hp, shift_);
          gl_points_.push_back(point_vbo(hp, radius));
      }
  }

  void draw_points() {
    // Draw input points and endpoints of the input segments.
    prepare_points();
    glUseProgram(gl_program_);
    glUniformMatrix4fv(mvp_matrix_location_, 1, GL_FALSE, projection_matrix_.data());
    std::array<float, 4> color{0.0f, 0.5f, 1.0f, 1.0f};
    glUniform4fv(color_location_, 1, color.data());
    for (const VBO& point_vbo : gl_points_) {
        glBindBuffer(GL_ARRAY_BUFFER, point_vbo.id_);
        glVertexAttribPointer(vertex_location_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(vertex_location_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)point_vbo.vertex_count_);
        glDisableVertexAttribArray(vertex_location_);
    }
  }

  void prepare_segments() {
      if (gl_segments_.id_ != 0) {
          return;
      }
      std::vector<GLPoint> segment_points;
      segment_points.reserve(segment_data_.size() * 2);
      for (std::size_t i = 0; i < segment_data_.size(); ++i) {
          point_type lp = low(segment_data_[i]);
          lp = deconvolve(lp, shift_);
          segment_points.emplace_back(lp.x(), lp.y());
          point_type hp = high(segment_data_[i]);
          hp = deconvolve(hp, shift_);
          segment_points.emplace_back(hp.x(), hp.y());
      }
      glGenBuffers(1, &gl_segments_.id_);
      glBindBuffer(GL_ARRAY_BUFFER, gl_segments_.id_);
      glBufferData(GL_ARRAY_BUFFER, segment_points.size() * sizeof(GLPoint), segment_points.data(), GL_STATIC_DRAW);
      gl_segments_.vertex_count_ = segment_points.size();
  }
  void draw_segments() {
    // Draw input segments.
    prepare_segments();
    glUseProgram(gl_program_);
    glUniformMatrix4fv(mvp_matrix_location_, 1, GL_FALSE, projection_matrix_.data());
    std::array<float, 4> color{0.0f, 0.5f, 1.0f, 1.0f};
    glUniform4fv(color_location_, 1, color.data());
    glBindBuffer(GL_ARRAY_BUFFER, gl_segments_.id_);
    glVertexAttribPointer(vertex_location_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glLineWidth(2.7f);
    glEnableVertexAttribArray(vertex_location_);
    glDrawArrays(GL_LINES, 0, (GLsizei)gl_segments_.vertex_count_);
    glDisableVertexAttribArray(vertex_location_);
  }

  void prepare_vertices() {
      static constexpr float radius = 3;
      if (!gl_vertices_.empty()) {
          return;
      }
      for (const_vertex_iterator it = vd_.vertices().begin();
           it != vd_.vertices().end(); ++it) {
          if (internal_edges_only_ && (it->color() == EXTERNAL_COLOR)) {
              continue;
          }
          point_type vertex(it->x(), it->y());
          vertex = deconvolve(vertex, shift_);
          gl_vertices_.push_back(point_vbo(vertex, radius));
      }
  }
  void draw_vertices() {
    // Draw voronoi vertices.
    prepare_vertices();
    glUseProgram(gl_program_);
    glUniformMatrix4fv(mvp_matrix_location_, 1, GL_FALSE, projection_matrix_.data());
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
    glUniform4fv(color_location_, 1, color.data());
    for (const VBO& vertex_vbo : gl_vertices_) {
      glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo.id_);
      glVertexAttribPointer(vertex_location_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
      glEnableVertexAttribArray(vertex_location_);
      glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)vertex_vbo.vertex_count_);
      glDisableVertexAttribArray(vertex_location_);
    }
  }

  void prepare_edges() {
      if (!gl_edges_.empty()) {
          return;
      }
      for (const_edge_iterator it = vd_.edges().begin();
           it != vd_.edges().end(); ++it) {
          if (primary_edges_only_ && !it->is_primary()) {
              continue;
          }
          if (internal_edges_only_ && (it->color() == EXTERNAL_COLOR)) {
              continue;
          }
          std::vector<point_type> samples;
          if (!it->is_finite()) {
              clip_infinite_edge(*it, &samples);
          } else {
              point_type vertex0(it->vertex0()->x(), it->vertex0()->y());
              samples.push_back(vertex0);
              point_type vertex1(it->vertex1()->x(), it->vertex1()->y());
              samples.push_back(vertex1);
              if (it->is_curved()) {
                  sample_curved_edge(*it, &samples);
              }
          }
          std::vector<GLPoint> gl_samples;
          gl_samples.reserve(samples.size());
          for (point_type& sample : samples) {
              point_type vertex = deconvolve(sample, shift_);
              gl_samples.emplace_back(sample.x(), sample.y());
          }
          GLuint vbo_id;
          glGenBuffers(1, &vbo_id);
          glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
          glBufferData(GL_ARRAY_BUFFER, gl_samples.size() * sizeof(GLPoint), gl_samples.data(), GL_STATIC_DRAW);
          gl_edges_.emplace_back(vbo_id, gl_samples.size());
      }
  }
  void draw_edges() {
    // Draw voronoi edges.
    prepare_edges();
    glUseProgram(gl_program_);
    glUniformMatrix4fv(mvp_matrix_location_, 1, GL_FALSE, projection_matrix_.data());
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
    glUniform4fv(color_location_, 1, color.data());
    glLineWidth(1.7f);
    for (const VBO& edge_vbo : gl_edges_) {
        glBindBuffer(GL_ARRAY_BUFFER, edge_vbo.id_);
        glVertexAttribPointer(vertex_location_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(vertex_location_);
        glDrawArrays(GL_LINES, 0, (GLsizei)edge_vbo.vertex_count_);
        glDisableVertexAttribArray(vertex_location_);
    }
  }

  void clip_infinite_edge(
      const edge_type& edge, std::vector<point_type>* clipped_edge) {
    const cell_type& cell1 = *edge.cell();
    const cell_type& cell2 = *edge.twin()->cell();
    point_type origin, direction;
    // Infinite edges could not be created by two segment sites.
    if (cell1.contains_point() && cell2.contains_point()) {
      point_type p1 = retrieve_point(cell1);
      point_type p2 = retrieve_point(cell2);
      origin.x((p1.x() + p2.x()) * 0.5);
      origin.y((p1.y() + p2.y()) * 0.5);
      direction.x(p1.y() - p2.y());
      direction.y(p2.x() - p1.x());
    } else {
      origin = cell1.contains_segment() ?
          retrieve_point(cell2) :
          retrieve_point(cell1);
      segment_type segment = cell1.contains_segment() ?
          retrieve_segment(cell1) :
          retrieve_segment(cell2);
      coordinate_type dx = high(segment).x() - low(segment).x();
      coordinate_type dy = high(segment).y() - low(segment).y();
      if ((low(segment) == origin) ^ cell1.contains_point()) {
        direction.x(dy);
        direction.y(-dx);
      } else {
        direction.x(-dy);
        direction.y(dx);
      }
    }
    coordinate_type side = xh(brect_) - xl(brect_);
    coordinate_type koef =
        side / (std::max)(fabs(direction.x()), fabs(direction.y()));
    if (edge.vertex0() == NULL) {
      clipped_edge->push_back(point_type(
          origin.x() - direction.x() * koef,
          origin.y() - direction.y() * koef));
    } else {
      clipped_edge->push_back(
          point_type(edge.vertex0()->x(), edge.vertex0()->y()));
    }
    if (edge.vertex1() == NULL) {
      clipped_edge->push_back(point_type(
          origin.x() + direction.x() * koef,
          origin.y() + direction.y() * koef));
    } else {
      clipped_edge->push_back(
          point_type(edge.vertex1()->x(), edge.vertex1()->y()));
    }
  }

  void sample_curved_edge(
      const edge_type& edge,
      std::vector<point_type>* sampled_edge) {
    coordinate_type max_dist = 1E-3 * (xh(brect_) - xl(brect_));
    point_type point = edge.cell()->contains_point() ?
        retrieve_point(*edge.cell()) :
        retrieve_point(*edge.twin()->cell());
    segment_type segment = edge.cell()->contains_point() ?
        retrieve_segment(*edge.twin()->cell()) :
        retrieve_segment(*edge.cell());
    voronoi_visual_utils<coordinate_type>::discretize(
        point, segment, max_dist, sampled_edge);
  }

  point_type retrieve_point(const cell_type& cell) {
    source_index_type index = cell.source_index();
    source_category_type category = cell.source_category();
    if (category == SOURCE_CATEGORY_SINGLE_POINT) {
      return point_data_[index];
    }
    index -= point_data_.size();
    if (category == SOURCE_CATEGORY_SEGMENT_START_POINT) {
      return low(segment_data_[index]);
    } else {
      return high(segment_data_[index]);
    }
  }

  segment_type retrieve_segment(const cell_type& cell) {
    source_index_type index = cell.source_index() - point_data_.size();
    return segment_data_[index];
  }

  point_type shift_;
  std::vector<point_type> point_data_;
  std::vector<segment_type> segment_data_;
  rect_type brect_;
  VB vb_;
  VD vd_;
  bool brect_initialized_;
  bool primary_edges_only_;
  bool internal_edges_only_;

  std::array<float, 16> projection_matrix_{};
  std::vector<VBO> gl_points_;
  VBO gl_segments_{0, 0};
  std::vector<VBO> gl_vertices_;
  std::vector<VBO> gl_edges_;
  GLuint gl_program_;
  GLuint vertex_shader_;
  GLuint fragment_shader_;
  GLint mvp_matrix_location_;
  GLint vertex_location_;
  GLint color_location_;
};

class MainWindow : public QWidget {
  Q_OBJECT

 public:
  MainWindow() {
    glWidget_ = new GLWidget();
    file_dir_ = QDir(QDir::currentPath(), tr("*.txt"));
    file_name_ = tr("");

    QHBoxLayout* centralLayout = new QHBoxLayout;
    centralLayout->addWidget(glWidget_);
    centralLayout->addLayout(create_file_layout());
    setLayout(centralLayout);

    update_file_list();
    setWindowTitle(tr("Voronoi Visualizer"));
    layout()->setSizeConstraint(QLayout::SetFixedSize);
  }

 private slots:
  void primary_edges_only() {
    glWidget_->show_primary_edges_only();
  }

  void internal_edges_only() {
    glWidget_->show_internal_edges_only();
  }

  void browse() {
    QString new_path = QFileDialog::getExistingDirectory(
        0, tr("Choose Directory"), file_dir_.absolutePath());
    if (new_path.isEmpty()) {
      return;
    }
    file_dir_.setPath(new_path);
    update_file_list();
  }

  void build() {
    file_name_ = file_list_->currentItem()->text();
    QString file_path = file_dir_.filePath(file_name_);
    message_label_->setText("Building...");
    glWidget_->build(file_path);
    message_label_->setText("Double click the item to build voronoi diagram:");
    setWindowTitle(tr("Voronoi Visualizer - ") + file_path);
  }

  void print_scr() {
    if (!file_name_.isEmpty()) {
      QImage screenshot = glWidget_->grabFramebuffer();
      QString output_file = file_dir_.absolutePath() + tr("/") +
          file_name_.left(file_name_.indexOf('.')) + tr(".png");
      screenshot.save(output_file, 0, -1);
    }
  }

 private:
  QGridLayout* create_file_layout() {
    QGridLayout* file_layout = new QGridLayout;

    message_label_ = new QLabel("Double click item to build voronoi diagram:");

    file_list_ = new QListWidget();
    file_list_->connect(file_list_,
                        SIGNAL(itemDoubleClicked(QListWidgetItem*)),
                        this,
                        SLOT(build()));

    QCheckBox* primary_checkbox = new QCheckBox("Show primary edges only.");
    connect(primary_checkbox, SIGNAL(clicked()),
        this, SLOT(primary_edges_only()));

    QCheckBox* internal_checkbox = new QCheckBox("Show internal edges only.");
    connect(internal_checkbox, SIGNAL(clicked()),
        this, SLOT(internal_edges_only()));

    QPushButton* browse_button =
        new QPushButton(tr("Browse Input Directory"));
    connect(browse_button, SIGNAL(clicked()), this, SLOT(browse()));
    browse_button->setMinimumHeight(50);

    QPushButton* print_scr_button = new QPushButton(tr("Make Screenshot"));
    connect(print_scr_button, SIGNAL(clicked()), this, SLOT(print_scr()));
    print_scr_button->setMinimumHeight(50);

    file_layout->addWidget(message_label_, 0, 0);
    file_layout->addWidget(file_list_, 1, 0);
    file_layout->addWidget(primary_checkbox, 2, 0);
    file_layout->addWidget(internal_checkbox, 3, 0);
    file_layout->addWidget(browse_button, 4, 0);
    file_layout->addWidget(print_scr_button, 5, 0);

    return file_layout;
  }

  void update_file_list() {
    QFileInfoList list = file_dir_.entryInfoList();
    file_list_->clear();
    if (file_dir_.count() == 0) {
      return;
    }
    QFileInfoList::const_iterator it;
    for (it = list.begin(); it != list.end(); it++) {
      file_list_->addItem(it->fileName());
    }
    file_list_->setCurrentRow(0);
  }

  QDir file_dir_;
  QString file_name_;
  GLWidget* glWidget_;
  QListWidget* file_list_;
  QLabel* message_label_;
};

int main(int argc, char* argv[]) {
  QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
  surfaceFormat.setSamples(4);
  surfaceFormat.setProfile(QSurfaceFormat::OpenGLContextProfile::CompatibilityProfile);
  QSurfaceFormat::setDefaultFormat(surfaceFormat);

  QApplication app(argc, argv);
  MainWindow window;
  window.show();
  return app.exec();
}

#include "voronoi_visualizer.moc"
