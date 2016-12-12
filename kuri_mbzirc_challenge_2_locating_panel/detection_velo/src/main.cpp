#include "ros/ros.h"
#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include "detection_velo/detection.h" 
#include "detection_velo/panel_searching.h"
#include <tf/transform_listener.h>
//using namespace velodyne_pointcloud;
int main(int argc, char **argv)
{
  ros::init(argc, argv, "detection_node");
  ros::NodeHandle node;
  detection Detection(node);
  ros::spin();
  return 0;
}
