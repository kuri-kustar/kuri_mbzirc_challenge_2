#include "ros/ros.h"
#include <iostream>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseArray.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/LaserScan.h>
#include <nav_msgs/Odometry.h>
#include <visualization_msgs/Marker.h>

#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <velodyne_pointcloud/point_types.h>

#include <laser_geometry/laser_geometry.h>

#include <pcl/point_cloud.h>
#include <pcl/common/common.h>
#include <pcl/common/transforms.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl_conversions/pcl_conversions.h>

#include <actionlib/server/simple_action_server.h>


#include <kuri_mbzirc_challenge_2_msgs/BoxPositionAction.h>
#include "../include/kuri_mbzirc_challenge_2_panel_detection/velodyne_box_detector.h"

#include <unistd.h>



BoxPositionActionHandler::BoxPositionActionHandler(std::string name) :
  as_(nh_, name, boost::bind(&BoxPositionActionHandler::executeCB, this, _1), false),
  action_name_(name),
  is_initiatializing_(false)
{
  detect_new_distance_ = 30; //Look for new clusters past this range

  pub_wall  = nh_.advertise<sensor_msgs::PointCloud2>("/explore/PCL", 10);
  pub_lines = nh_.advertise<visualization_msgs::Marker>("/explore/HoughLines", 10);
  pub_points= nh_.advertise<visualization_msgs::Marker>("/explore/points", 10);

  tf_listener = new tf::TransformListener();

  as_.start();
}

void BoxPositionActionHandler::executeCB(const GoalConstPtr &goal)
{
  if (goal->request == goal->REQUEST_START)
  {
    // Check inputs
    if (goal->range_max < goal->range_min)
    {
      printf("Error initializing minumum and maximum ranges (%f to %f)", goal->range_min, goal->range_max);
      setSuccess(false);
      return;
    }

    if (goal->angle_max < goal->angle_min)
    {
      printf("Error initializing minumum and maximum ranges (%f to %f)", goal->angle_min, goal->angle_max);
      setSuccess(false);
      return;
    }

    range_max_ = goal->range_max;
    range_min_ = goal->range_min;
    angle_max_ = goal->angle_max;
    angle_min_ = goal->angle_min;

    // Enable node
    is_initiatializing_ = true;

    // Enable callbacks
    sub_velo  = nh_.subscribe("/velodyne_points", 1, &BoxPositionActionHandler::callbackVelo, this);
    sub_odom  = nh_.subscribe("/odometry/filtered", 1, &BoxPositionActionHandler::callbackOdom, this);

    setSuccess(true);
  }

  else if (goal->request == goal->REQUEST_STOP)
  {
    // Unsubscribe topic handlers
    sub_velo.shutdown();
    sub_odom.shutdown();

    // Set return message
    setSuccess(true);
  }

  else if(goal->request == goal->REQUEST_QUERY)
  {
    // set the action state to succeeded
    setSuccess(true);
  }

  else
  {
    setSuccess(false);
  }

}

void BoxPositionActionHandler::setSuccess(bool success)
{
  result_.success = success;
  as_.setSucceeded(result_);
}



void BoxPositionActionHandler::callbackOdom(const nav_msgs::Odometry::ConstPtr& odom_msg)
{
  current_odom = *odom_msg;
}

void BoxPositionActionHandler::getInitialBoxClusters()
{
  // >>>>>>>>>
  // Initialize
  // >>>>>>>>>
  cluster_list.clear();
  bool check_angle = true;

  if (range_max_ == 0 && range_max_ == 0)
  {
    range_max_ = 60.0;
    range_min_ = 1.0;
  }

  if (angle_max_ == 0 && angle_min_ == 0)
  {
    angle_max_ = M_PI;
    angle_min_ = -M_PI;
    check_angle = false;
  }

  double laser_min_range = range_min_ * range_min_;
  double laser_max_range = range_max_ * range_max_;
  double laser_min_angle = angle_min_;
  double laser_max_angle = angle_max_;



  // Filter out points that are too close or too far, or out of range
  PcCloudPtr cloud_filtered (new PcCloud);

  for (int i=0; i<pc_current_->points.size(); i++)
  {
    PcPoint p = pc_current_->points[i];
    double r = p.x*p.x + p.y*p.y;
    if (r > laser_max_range || r < laser_min_range)
      continue;

    if (check_angle)
    {
      double angle = atan2(p.y, p.x);
      if (angle > laser_max_angle || angle < laser_min_angle)
        continue;
    }

    cloud_filtered->points.push_back (p);
  }
}

void BoxPositionActionHandler::callbackVelo(const sensor_msgs::PointCloud2::ConstPtr& cloud_msg)
{
  // Convert msg to pointcloud
  PcCloud cloud;

  pcl::fromROSMsg (*cloud_msg, cloud);
  pc_current_ = cloud.makeShared();


  if (is_initiatializing_)
  {
    getInitialBoxClusters();
    return;
  }

  PcCloudPtr cloud_filtered = pc_current_;

  if (cloud_filtered->points.size() == 0)
  {
    std::cout << "No laser points detected nearby.\n";
    //setSuccess(false);
    return;
  }


  // Get clusters
  PcCloudPtrList pc_vector;
  getCloudClusters(cloud_filtered, pc_vector);


  // Get size of each cluster
  std::vector<Eigen::Vector3f> dimension_list;
  std::vector<Eigen::Vector4f> centroid_list;
  std::vector<std::vector<PcPoint> > corners_list;
  computeBoundingBox(pc_vector, dimension_list, centroid_list, corners_list);


  // Only keep the clusters that are likely to be panels
  PcCloudPtrList pc_vector_clustered;
  for (int i = 0; i< dimension_list.size(); i++)
  {
    if (dimension_list[i][2] <= 1.5 && dimension_list[i][1] <= 1.5)
      pc_vector_clustered.push_back(pc_vector[i]);
  }


  if (pc_vector_clustered.size() == 0)
  {
    std::cout << "Could not find panel cluster.\n";
    //setSuccess(false);
    return;
  }

  // Publish cluster clouds
  for (int i=0; i<pc_vector_clustered.size(); i++)
  {
    sensor_msgs::PointCloud2 cloud_cluster_msg;
    pcl::toROSMsg(*pc_vector_clustered[i], cloud_cluster_msg);
    cloud_cluster_msg.header.frame_id = cloud_msg->header.frame_id;
    cloud_cluster_msg.header.stamp = ros::Time::now();
    pub_wall.publish(cloud_cluster_msg);

    if (pc_vector_clustered.size() > 1)
      usleep(200*1000);
  }

  /* Select one cloud */
  if (pc_vector_clustered.size() > 1)
  {
    std::cout << "Found multiple panel clusters. Using the first one.\n";
  }

  PcCloudPtr cluster_cloud = pc_vector_clustered[0];

  is_initiatializing_ = false;

}

void BoxPositionActionHandler::computeBoundingBox(PcCloudPtrList& pc_vector,std::vector<Eigen::Vector3f>& dimension_list, std::vector<Eigen::Vector4f>& centroid_list, std::vector<std::vector<PcPoint> >& corners)
{
  PcCloudPtr cloud_plane (new PcCloud ());
  Eigen::Vector3f one_dimension;

  for (PcCloudPtrList::const_iterator iterator = pc_vector.begin(), end = pc_vector.end(); iterator != end; ++iterator)
  {
    cloud_plane=*iterator;

    // Compute principal directions
    Eigen::Vector4f pcaCentroid;
    pcl::compute3DCentroid(*cloud_plane, pcaCentroid);

    Eigen::Matrix3f covariance;
    computeCovarianceMatrixNormalized(*cloud_plane, pcaCentroid, covariance);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen_solver(covariance, Eigen::ComputeEigenvectors);
    Eigen::Matrix3f eigenVectorsPCA = eigen_solver.eigenvectors();
    eigenVectorsPCA.col(2) = eigenVectorsPCA.col(0).cross(eigenVectorsPCA.col(1));

    // Transform the original cloud to the origin where the principal components correspond to the axes.
    Eigen::Matrix4f projectionTransform(Eigen::Matrix4f::Identity());
    projectionTransform.block<3,3>(0,0) = eigenVectorsPCA.transpose();
    projectionTransform.block<3,1>(0,3) = -1.f * (projectionTransform.block<3,3>(0,0) * pcaCentroid.head<3>());
    PcCloudPtr cloudPointsProjected (new PcCloud);



    pcl::transformPointCloud(*cloud_plane, *cloudPointsProjected, projectionTransform);

    // Get the minimum and maximum points of the transformed cloud.
    PcPoint minPoint, maxPoint;

    pcl::getMinMax3D(*cloudPointsProjected, minPoint, maxPoint);

    // save the centroid into the centroid vector
    centroid_list.push_back(pcaCentroid);
    // save dimenstion into dimension list
    one_dimension[0] = maxPoint.x - minPoint.x;
    one_dimension[1] = maxPoint.y - minPoint.y;
    one_dimension[2] = maxPoint.z - minPoint.z;
    dimension_list.push_back(one_dimension);


    // Transform back
    Eigen::Matrix4f bboxTransform(Eigen::Matrix4f::Identity());
    bboxTransform.block<3,3>(0,0) = eigenVectorsPCA;

    PcCloudPtr cloud_corners_pca  (new PcCloud);
    PcCloudPtr cloud_corners_base (new PcCloud);

    cloud_corners_pca->points.push_back(minPoint);
    cloud_corners_pca->points.push_back(maxPoint);

    for (int i=0; i<cloud_corners_pca->points.size(); i++)
    {
      cloud_corners_pca->points[i].x -= projectionTransform(0,3);
      cloud_corners_pca->points[i].y -= projectionTransform(1,3);
      cloud_corners_pca->points[i].z -= projectionTransform(2,3);
    }

    pcl::transformPointCloud(*cloud_corners_pca, *cloud_corners_base, bboxTransform);

    // Extract corners
    std::vector<PcPoint> c;
    for (int i=0; i<cloud_corners_base->points.size(); i++)
      c.push_back(cloud_corners_base->points[i]);

    // Save list of corners
    corners.push_back(c);
  }
}

void BoxPositionActionHandler::drawPoints(std::vector<geometry_msgs::Point> points, std::string frame_id)
{
  // Publish
  visualization_msgs::Marker marker_msg;
  marker_msg.header.frame_id = frame_id;
  marker_msg.header.stamp = ros::Time::now();
  marker_msg.type = marker_msg.POINTS;

  marker_msg.scale.x = 0.05;
  marker_msg.scale.y = 0.05;
  marker_msg.color.a = 1.0;
  marker_msg.color.g = 1.0;

  marker_msg.points = points;

  pub_points.publish(marker_msg);
}

void BoxPositionActionHandler::getCloudClusters(PcCloudPtr cloud_ptr, PcCloudPtrList& pc_vector)
{
  // Creating the KdTree object for the search method of the extraction
  pcl::search::KdTree<PcPoint>::Ptr tree (new pcl::search::KdTree<PcPoint>);
  tree->setInputCloud (cloud_ptr);

  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<PcPoint> ec;
  ec.setClusterTolerance (0.5); // 50cm - big since we're sure the panel is far from other obstacles (ie. barriers)
  ec.setMinClusterSize (3);
  ec.setMaxClusterSize (1000);
  ec.setSearchMethod (tree);
  ec.setInputCloud (cloud_ptr);
  ec.extract (cluster_indices);

  // Get the cloud representing each cluster
  for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it)
  {
    PcCloudPtr cloud_cluster (new PcCloud);
    for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); ++pit)
      cloud_cluster->points.push_back (cloud_ptr->points[*pit]);

    cloud_cluster->width = cloud_cluster->points.size ();
    cloud_cluster->height = 1;
    cloud_cluster->is_dense = true;

    pc_vector.push_back(cloud_cluster);
  }
}


PcCloudPtrList extractBoxClusters(PcCloudPtr cloud_ptr)
{

}


Eigen::Matrix4d BoxPositionActionHandler::convertStampedTransform2Matrix4d(tf::StampedTransform t)
{
  // Get translation
  Eigen::Vector3d T1(
      t.getOrigin().x(),
      t.getOrigin().y(),
      t.getOrigin().z()
  );

  // Get rotation matrix
  tf::Quaternion qt = t.getRotation();
  tf::Matrix3x3 R1(qt);

  Eigen::Matrix3d R;
  tf::matrixTFToEigen(R1,R);

  // Set
  Eigen::Matrix4d tf_eigen;
  tf_eigen.setZero ();
  tf_eigen.block (0, 0, 3, 3) = R;
  tf_eigen.block (0, 3, 3, 1) = T1;
  tf_eigen (3, 3) = 1;

  return tf_eigen;
}

std::vector<double> BoxPositionActionHandler::generateRange(double start, double end, double step)
{
  std::vector<double> vec;

  vec.push_back(start);

  while(1)
  {
    start += step;
    vec.push_back(start);

    if (start > end)
      break;
  }

  return vec;
}

geometry_msgs::Quaternion BoxPositionActionHandler::getQuaternionFromYaw(double yaw)
{
  geometry_msgs::Quaternion quat;

  tf::Quaternion tf_q;
  tf_q = tf::createQuaternionFromYaw(yaw);

  quat.x = tf_q.getX();
  quat.y = tf_q.getY();
  quat.z = tf_q.getZ();
  quat.w = tf_q.getW();

  return quat;
}



int main(int argc, char **argv)
{
  ros::init(argc, argv, "box_location");
  ros::NodeHandle node;

  // Action server
  BoxPositionActionHandler *action_handler;
  std::string actionlib_topic = "get_box_cluster";
  action_handler = new BoxPositionActionHandler(actionlib_topic);

  std::cout << "Waiting for messages on the \"" << actionlib_topic << "\" actionlib topic\n";

  ros::spin();
  return 0;
}




