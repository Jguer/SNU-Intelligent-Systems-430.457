// state definition
#define INIT 0
#define RUNNING 1
#define MAPPING 2
#define PATH_PLANNING 3
#define FINISH -1

#include <ackermann_msgs/AckermannDriveStamped.h>
#include <cmath>
#include <gazebo_msgs/ModelStates.h>
#include <gazebo_msgs/SetModelState.h>
#include <gazebo_msgs/SpawnModel.h>
#include <project2/pid.h>
#include <project2/rrtTree.h>
#include <pwd.h>
#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <unistd.h>

// map spec
cv::Mat map;
double res;
int map_y_range;
int map_x_range;
double map_origin_x;
double map_origin_y;
double world_x_min;
double world_x_max;
double world_y_min;
double world_y_max;

// parameters you should adjust : K, margin, MaxStep
int margin = 15;
int K = 1500;
double MaxStep = 4;

// way points
std::vector<point> waypoints;

// path
// std::vector<point> path_RRT;
std::vector<traj> path_RRT;

// control
// std::vector<control> control_RRT;

// robot
point robot_pose;
ackermann_msgs::AckermannDriveStamped cmd;
double speed;
double angle;
//
// at full joystick depression you'll go this fast
double max_speed = 2.00;
double max_turn = 60.0 * M_PI / 180.0;

// FSM state
int state;

// function definition
void set_waypoints();
void generate_path_RRT();
void callback_state(gazebo_msgs::ModelStatesConstPtr msgs);
void setcmdvel(double v, double w);

int main(int argc, char **argv) {
  ros::init(argc, argv, "rrt_main");
  ros::NodeHandle n;

  // Initialize topics
  ros::Subscriber gazebo_pose_sub =
      n.subscribe("/gazebo/model_states", 100, callback_state);
  ros::Publisher cmd_vel_pub =
      n.advertise<ackermann_msgs::AckermannDriveStamped>(
          "/vesc/low_level/ackermann_cmd_mux/output", 100);
  ros::ServiceClient gazebo_spawn =
      n.serviceClient<gazebo_msgs::SpawnModel>("/gazebo/spawn_urdf_model");
  ros::ServiceClient gazebo_set =
      n.serviceClient<gazebo_msgs::SetModelState>("/gazebo/set_model_state");
  printf("Initialize topics\n");

  // Load Map

  char *user = getpwuid(getuid())->pw_name;
  map = cv::imread(
      (std::string("/home/") + std::string(user) +
       std::string("/catkin_ws/src/project2/src/ground_truth_map.pgm"))
          .c_str(),
      CV_LOAD_IMAGE_GRAYSCALE);

  map_y_range = map.cols;
  map_x_range = map.rows;
  map_origin_x = map_x_range / 2.0 - 0.5;
  map_origin_y = map_y_range / 2.0 - 0.5;
  world_x_min = -10.0;
  world_x_max = 10.0;
  world_y_min = -10.0;
  world_y_max = 10.0;
  res = 0.05;
  printf("Load map\n");

  if (!map.data) {
    printf("Could not open or find the image\n");
    return -1;
  }

  // Set Way Points
  set_waypoints();
  printf("Set way points\n");

  // RRT
  generate_path_RRT();
  printf("Generate RRT\n");

  // FSM
  state = INIT;
  bool running = true;
  int look_ahead_idx;
  PID *pid_ctrl = new PID(0.6, 0.3, 0.0);
  ros::Rate control_rate(60);

  while (running) {
    switch (state) {
    case INIT: {
      look_ahead_idx = 0;
      printf("path size : %lu\n", path_RRT.size());
      // visualize path

      for (int i = 0; i < path_RRT.size(); i++) {
        gazebo_msgs::SpawnModel model;
        model.request.model_xml =
            std::string("<robot name=\"simple_ball\">") +
            std::string("<static>true</static>") +
            std::string("<link name=\"ball\">") + std::string("<inertial>") +
            std::string("<mass value=\"1.0\" />") +
            std::string("<origin xyz=\"0 0 0\" />") +
            std::string("<inertia  ixx=\"1.0\" ixy=\"1.0\"  ixz=\"1.0\"  "
                        "iyy=\"1.0\"  iyz=\"1.0\"  izz=\"1.0\" />") +
            std::string("</inertial>") + std::string("<visual>") +
            std::string("<origin xyz=\"0 0 0\" rpy=\"0 0 0\" />") +
            std::string("<geometry>") +
            std::string("<sphere radius=\"0.09\"/>") +
            std::string("</geometry>") + std::string("</visual>") +
            std::string("<collision>") +
            std::string("<origin xyz=\"0 0 0\" rpy=\"0 0 0\" />") +
            std::string("<geometry>") +
            std::string("<sphere radius=\"0.09\"/>") +
            std::string("</geometry>") + std::string("</collision>") +
            std::string("</link>") +
            std::string("<gazebo reference=\"ball\">") +
            std::string("<mu1>10</mu1>") + std::string("<mu2>10</mu2>") +
            std::string("<material>Gazebo/Blue</material>") +
            std::string("<turnGravityOff>true</turnGravityOff>") +
            std::string("</gazebo>") + std::string("</robot>");

        std::ostringstream ball_name;
        ball_name << i;
        model.request.model_name = ball_name.str();
        model.request.reference_frame = "world";
        model.request.initial_pose.position.x = path_RRT[i].x;
        model.request.initial_pose.position.y = path_RRT[i].y;
        model.request.initial_pose.position.z = 0.7;
        model.request.initial_pose.orientation.w = 0.0;
        model.request.initial_pose.orientation.x = 0.0;
        model.request.initial_pose.orientation.y = 0.0;
        model.request.initial_pose.orientation.z = 0.0;
        gazebo_spawn.call(model);
        ros::spinOnce();
      }
      printf("Spawn path\n");

      // initialize robot position
      geometry_msgs::Pose model_pose;
      model_pose.position.x = waypoints[0].x;
      model_pose.position.y = waypoints[0].y;
      model_pose.position.z = 0.3;
      model_pose.orientation.x = 0.0;
      model_pose.orientation.y = 0.0;
      model_pose.orientation.z = 0.0;
      model_pose.orientation.w = 1.0;

      geometry_msgs::Twist model_twist;
      model_twist.linear.x = 0.0;
      model_twist.linear.y = 0.0;
      model_twist.linear.z = 0.0;
      model_twist.angular.x = 0.0;
      model_twist.angular.y = 0.0;
      model_twist.angular.z = 0.0;

      gazebo_msgs::ModelState modelstate;
      modelstate.model_name = "racecar";
      modelstate.reference_frame = "world";
      modelstate.pose = model_pose;
      modelstate.twist = model_twist;

      gazebo_msgs::SetModelState setmodelstate;
      setmodelstate.request.model_state = modelstate;

      gazebo_set.call(setmodelstate);
      ros::spinOnce();
      ros::Rate(0.33).sleep();

      printf("Initialize ROBOT\n");
      state = RUNNING;
    } break;
    case RUNNING: {
      if (path_RRT.size() == 0) {
        printf("Path is empty.\n");
        state = FINISH;
        continue;
      }
      speed =
          2.0 - 1.5 / (1.0 + (robot_pose.distance(path_RRT[look_ahead_idx].x,
                                                  path_RRT[look_ahead_idx].y)));
      angle = pid_ctrl->get_control(robot_pose, path_RRT[look_ahead_idx]);

      // Validate Speed
      speed = (speed > max_speed) ? max_speed : speed;
      speed = (speed < -max_speed) ? -max_speed : speed;
      // Validate Angle
      angle = (angle > max_turn) ? max_turn : angle;
      angle = (angle < -max_turn) ? -max_turn : angle;

      setcmdvel(speed, angle);
      printf("Debug Parameters\n");
      printf("Speed, Angle : %.2f, %.2f \n", speed, angle);
      printf("Car Pose : %.2f,%.2f,%.2f,%.2f,%.2f \n", robot_pose.x,
             robot_pose.y, robot_pose.th, angle, path_RRT[look_ahead_idx].th);
      cmd_vel_pub.publish(cmd);

      double dist_to_target = robot_pose.distance(path_RRT[look_ahead_idx].x,
                                                  path_RRT[look_ahead_idx].y);

      printf("%fs\n", dist_to_target);
      if (dist_to_target <= 0.2) {
        printf("New destination Point\n");
        look_ahead_idx++;
        if (look_ahead_idx == path_RRT.size()) {
          state = FINISH;
        }
      }

      ros::spinOnce();
      control_rate.sleep();
    } break;
    case FINISH: {
      setcmdvel(0, 0);
      cmd_vel_pub.publish(cmd);
      running = false;
      ros::spinOnce();
      control_rate.sleep();
    } break;
    default: { } break; }
  }
  return 0;
}

void generate_path_RRT() {
  for (int i = 0; i < waypoints.size() - 1; i++) {
    rrtTree *tree = new rrtTree(waypoints.at(i), waypoints.at(i + 1), map,
                                map_origin_x, map_origin_y, res, margin);
    printf("New rrtTree generated.\n");

    std::vector<traj> path_tmp = tree->generateRRT(
        world_x_max, world_x_min, world_y_max, world_y_min, K, MaxStep);
    printf("New trajectory generated.\n");
    tree->visualizeTree(path_tmp);
    path_RRT.insert(path_RRT.end(), path_tmp.begin(), path_tmp.end());
  }
}

void set_waypoints() {
  point waypoint_candid[4];
  waypoint_candid[0].x = 5.0;
  waypoint_candid[0].y = -8.0;
  waypoint_candid[1].x = -6.0;
  waypoint_candid[1].y = -7.0;
  waypoint_candid[2].x = -7.0;
  waypoint_candid[2].y = 6.0;
  waypoint_candid[3].x = 3.0;
  waypoint_candid[3].y = 7.0;
  waypoint_candid[3].th = 0.0;

  int order[] = {3, 1, 2, 3};
  int order_size = 3;

  for (int i = 0; i < order_size; i++) {
    waypoints.push_back(waypoint_candid[order[i]]);
  }
}

void callback_state(gazebo_msgs::ModelStatesConstPtr msgs) {
  for (int i = 0; i < msgs->name.size(); i++) {
    if (std::strcmp(msgs->name[i].c_str(), "racecar") == 0) {
      robot_pose.x = msgs->pose[i].position.x;
      robot_pose.y = msgs->pose[i].position.y;
      robot_pose.th = tf::getYaw(msgs->pose[i].orientation);
    }
  }
}

void setcmdvel(double v, double w) {
  cmd.drive.speed = v;
  cmd.drive.steering_angle = w;
}
