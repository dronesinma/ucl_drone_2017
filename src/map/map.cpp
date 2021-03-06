/*
 *  This file is part of ucl_drone 2017.
 *  For more information, please refer
 *  to the corresponding header file.
 *
 *  \author Boris Dehem
 *  \date 2017
 *
 */

#include <ucl_drone/map/map.h>

Map::Map() {}

Map::Map(ros::NodeHandle* nh) : cloud(new pcl::PointCloud< pcl::PointXYZ >())
{
  cv::initModule_nonfree();  // initialize OpenCV SIFT and SURF

  nh = nh;
  bundle_channel = nh->resolveName("bundle");
  bundle_pub     = nh->advertise<ucl_drone::BundleMsg>(bundle_channel, 1);

  benchmark_channel = nh->resolveName("benchmark");
  benchmark_pub     = nh->advertise<ucl_drone::BenchmarkInfoMsg>(benchmark_channel, 1);

  //Get some parameters from launch file
  ros::param::get("~thresh_descriptor_match", thresh_descriptor_match);
  ros::param::get("~max_matches", max_matches);
  ros::param::get("~no_bundle_adjustment", no_bundle_adjustment);
  ros::param::get("~only_init", only_init);
  ros::param::get("~outlier_threshold", outlier_threshold);
  ros::param::get("~manual_keyframes", manual_keyframes);
  ros::param::get("~sonar_unavailable", sonar_unavailable);
  ros::param::get("~n_kf_local_ba", n_kf_local_ba);
  ros::param::get("~freq_global_ba", freq_global_ba);

  ros::param::get("~min_dist", min_dist);
  ros::param::get("~min_time", min_time);
  ros::param::get("~inliers_thresh", inliers_thresh);
  ros::param::get("~FOV_thresh", FOV_thresh);
  ros::param::get("~time_thresh", time_thresh);
  ros::param::get("~dist_thresh", dist_thresh);

  ROS_INFO("dist_thresh = %f", dist_thresh);
  ROS_INFO("init map");

  is_adjusting_bundle     = false;
  n_inliers_moving_avg    = 0;
  kf_since_last_global_BA = 0;

  camera = Camera(true);

  // get camera parameters in launch file
  if (!Read::CamMatrixParams("cam_matrix")) ROS_ERROR("cam_matrix not properly transmitted");
  if (!Read::ImgSizeParams("img_size"))     ROS_ERROR("img_size not properly transmitted");

  // initialize empty opencv vectors
  this->tvec = cv::Mat::zeros(3, 1, CV_64FC1);
  this->rvec = cv::Mat::zeros(3, 1, CV_64FC1);

  ROS_DEBUG("map started");
}

Map::~Map() {reset();}

void Map::reset()
{
  std::map<int,Keyframe*>::iterator it_k;
  std::map<int,Landmark*>::iterator it_l;
  for(it_k = keyframes.begin(); it_k!=keyframes.end();++it_k) delete it_k->second;
  for(it_l = landmarks.begin(); it_l!=landmarks.end();++it_l) delete it_l->second;
  keyframes.clear();
  landmarks.clear();
  cloud = boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ> >(new pcl::PointCloud<pcl::PointXYZ>);
  tvec = cv::Mat::zeros(3, 1, CV_64FC1);
  rvec = cv::Mat::zeros(3, 1, CV_64FC1);
}

bool Map::isInitialized(){  return (keyframes.size() > 3);}

int Map::addPoint(cv::Point3d& coordinates, cv::Mat& descriptor)
{
  Landmark* new_landmark = new Landmark(coordinates, descriptor);
  landmarks[new_landmark->ID] = new_landmark;
  pcl::PointXYZ new_point;
  new_point.x = coordinates.x;
  new_point.y = coordinates.y;
  new_point.z = coordinates.z;
  cloud->points.push_back(new_point);
  descriptors.push_back(descriptor);
  landmark_IDs.push_back(new_landmark->ID);
  return new_landmark->ID;
}

void Map::updatePoint(int ptID, cv::Point3d coordinates)
{
  std::map<int,Landmark*>::iterator it = landmarks.find(ptID);
  if (it==landmarks.end())
  {
    ROS_INFO("Trying to update point %d, but it doesn't exist",ptID);
    return;
  }
  it->second->updateCoords(coordinates);
  int idx = std::distance(landmarks.begin(),it);
  pcl::PointXYZ new_point;
  new_point.x = coordinates.x;
  new_point.y = coordinates.y;
  new_point.z = coordinates.z;
  cloud->points[idx] = new_point;
}


void Map::removePoint(int ptID)
{
  bool keyframe_is_dead;
  std::map<int,Landmark*>::iterator it = landmarks.find(ptID);
  if(it==landmarks.end())
  {
    ROS_INFO("Trying to remove point %d but it doesn't exist",ptID);
    return;
  }
  int idx = std::distance(landmarks.begin(),it);
  landmark_IDs.erase(landmark_IDs.begin()+idx);
  Landmark* lm = it->second;
  std::set<int>::iterator it2;
  for (it2 = lm->keyframes_seeing.begin(); it2!=lm->keyframes_seeing.end();++it2)
  {
    ROS_DEBUG("removing point %d from keyframe %d",ptID,*it2);
    keyframe_is_dead = keyframes[*it2]->removePoint(ptID);
    if (keyframe_is_dead&&(keyframes.size()>1))
      removeKeyframe(*it2);
  }
  delete lm;
  landmarks.erase(ptID);
  cloud->erase(cloud->begin() + idx);

  // Removing a row from descriptors
  cv::Mat temp(descriptors.rows - 1,descriptors.cols, descriptors.type());
  cv::Mat t1;   // Result
  if (idx > 0)  // Copy everything above that one idx.
  {
    cv::Rect rect(0, 0, descriptors.cols, idx);
    descriptors(rect).copyTo(temp(rect));
  }

  if (idx < descriptors.rows-1) // Copy everything below that one idx.
  {
    cv::Rect rect1(0, idx+1, descriptors.cols, descriptors.rows - idx - 1 );
    cv::Rect rect2(0, idx,   descriptors.cols, descriptors.rows - idx - 1 );
    descriptors(rect1).copyTo(temp(rect2));
  }
  descriptors = temp;
}

void Map::removeKeyframe(int kfID)
{
  ROS_INFO("removing keyframe %d",kfID);
  std::map<int,Keyframe*>::iterator it = keyframes.find(kfID);
  bool point_is_dead;
  if(it==keyframes.end())
  {
    ROS_INFO("Trying to remove keyframe %d but it doesn't exist",kfID);
    return;
  }
  Keyframe* kf = it->second;
  for (int i = 0; i < kf->point_IDs.size(); i++)
  {
    int lmID = it->second->point_IDs[i];
    if (lmID >= 0)
    {
      ROS_INFO("removing point %d from keyframe %d",lmID,kfID);
      point_is_dead = landmarks[lmID]->setAsUnseenBy(kfID);
      if (point_is_dead) removePoint(lmID);
    }
  }
  keyframes.erase(kfID);
  delete kf;
}


void Map::setPointAsSeen(int ptID, int kfID, int idx_in_kf)
{
  ROS_DEBUG("setting point %d as seen by keyframe %d",ptID,kfID);
  landmarks[ptID]->setAsSeenBy(kfID);
  keyframes[kfID]->setAsSeeing(ptID, idx_in_kf);
}

void Map::matchKeyframes(Keyframe* kf0, Keyframe* kf1)
{
  if (kf0->descriptors.rows == 0 || kf1->descriptors.rows == 0)
    return;
  int i, nmatch, ptID, ptID_kf0, ptID_kf1, n_new_pts;
  cv::Point3d point3D;
  std::vector<int> idx_kf0, idx_kf1;
  matchDescriptors(kf0->descriptors, kf1->descriptors, kf0->point_IDs, kf1->point_IDs, idx_kf0, idx_kf1, thresh_descriptor_match, max_matches);
  nmatch = idx_kf0.size();
  n_new_pts = 0;
  for (i = 0; i<nmatch; i++)
  {
    ROS_DEBUG("indices of match %d are %d and %d",i,idx_kf0[i],idx_kf1[i]);
    ptID_kf0 = kf0->point_IDs[idx_kf0[i]];
    ptID_kf1 = kf1->point_IDs[idx_kf1[i]];
    ROS_DEBUG("IDs of match %d are %d and %d",i,ptID_kf0,ptID_kf1);
    if ((ptID_kf0==-1) && (ptID_kf1==-1))
    {
      n_new_pts++;
      triangulate(point3D, kf0, kf1, idx_kf0[i], idx_kf1[i]);
      if (pointIsVisible(*kf0,point3D,-0.5) && pointIsVisible(*kf1,point3D,-0.5))
      {
        cv::Mat new_descriptor = 0.5*(kf0->descriptors.row(idx_kf0[i])+kf1->descriptors.row(idx_kf1[i]));
        ptID = addPoint(point3D, new_descriptor);
        setPointAsSeen(ptID, kf0->ID, idx_kf0[i]);
        setPointAsSeen(ptID, kf1->ID, idx_kf1[i]);
      }
    }
    else if ((ptID_kf0==-1)&&(ptID_kf1!=-2))
    {
      ROS_WARN("anomaly1");
      setPointAsSeen(ptID_kf1, kf0->ID, idx_kf0[i]);
    }
    else if ((ptID_kf1==-1)&&(ptID_kf0!=-2))
    {
      ROS_WARN("anomaly2");
      setPointAsSeen(ptID_kf0, kf1->ID, idx_kf1[i]);
    }
  }
  ROS_INFO("Matching keyframe %d with keyframe %d. There are %d new points",kf0->ID, kf1->ID, n_new_pts);
}

void Map::matchKeyframeWithMap(Keyframe* kf)
{
  if (kf->descriptors.rows == 0 || this->descriptors.rows == 0)
    return;
  int i, nmatch, ptID, pt_ID_kf, pt_idx_kf, pt_idx_map, pt_ID, n_kf_seeing;
  cv::Point3d point3D;
  std::vector<int> map_indices, keyframe_indices;
  matchDescriptors(descriptors, kf->descriptors, map_indices, keyframe_indices, DIST_THRESHOLD,-1);

  nmatch = keyframe_indices.size();
  for (i = 0; i<nmatch; i++)
  {
    pt_idx_kf = keyframe_indices[i];
    pt_ID_kf  = kf->point_IDs[pt_idx_kf];
    if (pt_ID_kf==-1)
    {
      pt_idx_map = map_indices[i];
      pt_ID      = landmark_IDs[pt_idx_map];
      n_kf_seeing = landmarks[pt_ID]->keyframes_seeing.size();
      //descriptors.row(pt_idx_map) = (n_kf_seeing*descriptors.row(pt_idx_map) + kf->descriptors.row(pt_idx_kf))/(n_kf_seeing+1);
      setPointAsSeen(pt_ID, kf->ID, pt_idx_kf);
    }
    else
    {
      ROS_WARN("anomaly3");
    }
  }
  ROS_INFO("Matching keyframe %d with map. There are %d matching points",kf->ID, nmatch);
}

void Map::newKeyframe(Frame& frame)
{
  ROS_INFO("new keyframe start pose = %f, %f, %f",frame.pose.x,frame.pose.y,frame.pose.z);
  if (frame.img_points.size() <= 10)
  {
    ROS_INFO("I want to create a new keyframe, but current frame only has %lu points",frame.img_points.size());
    return;
  }
  Keyframe* new_keyframe = new Keyframe(frame,&camera);
  last_new_keyframe = ros::Time::now();
  keyframes[new_keyframe->ID] = new_keyframe;
  if (keyframes.size() < 2) return;
  if (n_kf_local_ba <= 0 || keyframes.size() <= n_kf_local_ba || keyframes.size() % freq_global_ba == 0)
    first_kf_to_adjust = keyframes.begin();
  else
  {
    first_kf_to_adjust = keyframes.end();
    std::advance(first_kf_to_adjust,-n_kf_local_ba);
  }
  std::vector<int> keyframes_to_adjust;
  std::map<int,Keyframe*>::iterator it;
  matchKeyframeWithMap(new_keyframe);
  for (it = first_kf_to_adjust; it!=keyframes.end(); ++it)
  {
    if (it->first != new_keyframe->ID)
    {
      matchKeyframes(new_keyframe, it->second);
    }
    keyframes_to_adjust.push_back(it->first); //add all kfs
  }

  ROS_INFO("\t Map now has %lu points",this->cloud->points.size());
  doBundleAdjustment(keyframes_to_adjust, false);
}


bool Map::processFrame(Frame& frame, ucl_drone::Pose3D& PnP_pose)
{
  int n_keyframes = keyframes.size();
  int n_inliers;
  double fraction_FOV_without_inliers;

  int PnP_result = doPnP(frame, PnP_pose, n_inliers, fraction_FOV_without_inliers);
  n_inliers_moving_avg = (2*n_inliers_moving_avg + n_inliers)/3;

  if (PnP_result)
  {
    frame.pose.x    = PnP_pose.x;
    frame.pose.y    = PnP_pose.y;
    frame.pose.rotZ = PnP_pose.rotZ;
    if (sonar_unavailable&&isInitialized())
    {
      frame.pose.z    = PnP_pose.z;
      frame.pose.rotX = PnP_pose.rotX;
      frame.pose.rotY = PnP_pose.rotY;
    }
  }

  if (manual_keyframes)
  {
    if (manual_pose_available)
    {
      frame.pose.z    = manual_pose.z;
      frame.pose.rotX = manual_pose.rotX;
      frame.pose.rotY = manual_pose.rotY;
      newKeyframe(frame);
      manual_pose_available = false;
    }
  }
  else if (manual_pose_available&&!isInitialized())
  {
    ROS_INFO("keyframe needed because manual received (% 4.2f, % 4.2f, % 4.2f)",manual_pose.x,manual_pose.y,manual_pose.z);
    frame.pose.z    = manual_pose.z;
    frame.pose.rotX = manual_pose.rotX;
    frame.pose.rotY = manual_pose.rotY;
    newKeyframe(frame);
    manual_pose_available = false;
  }
  else if (keyframeNeeded(manual_pose_available, n_inliers, fraction_FOV_without_inliers, frame.pose))
  {
    if (keyframes.size()==0)
    {  frame.pose.x = 0; frame.pose.y = 0; frame.pose.z = 0; frame.pose.rotX = 0; frame.pose.rotY = 0; frame.pose.rotZ = 0;  }
    newKeyframe(frame);
    manual_pose_available = false;
  }
  switch(PnP_result){
    case 1  : //PnP successful
      ROS_INFO_THROTTLE(4,"(%3d inliers) PnP_pose is: x = % 4.2f, rotX = % 7.1f", n_inliers, PnP_pose.x, PnP_pose.rotX*180/PI);
      ROS_INFO_THROTTLE(4,"                         : y = % 4.2f, rotY = % 7.1f", PnP_pose.y, PnP_pose.rotY*180/PI);
      ROS_INFO_THROTTLE(4,"                         : z = % 4.2f, rotZ = % 7.1f", PnP_pose.z, PnP_pose.rotZ*180/PI);
      ROS_INFO_THROTTLE(4,"                                                    ");
      return true;
    case -1  : //Empty frame
      ROS_INFO_THROTTLE(4,"Frame is empty");
      return false;
    case -2  : //Empty map
      ROS_INFO_THROTTLE(4,"Map is empty");
      return false;
    case -3  :
      ROS_INFO_THROTTLE(4,"Tracking lost! not enough matching points");
      return false;
    case -4  :
      ROS_INFO_THROTTLE(4,"Tracking lost! not enough inliers");
      return false;
    case -5 :
      ROS_INFO_THROTTLE(4,"TRACKING LOST ! (Determinant of rotation matrix)");
      return false;
    case -6 :
      ROS_INFO_THROTTLE(4,"TRACKING LOST ! (PnP found bad symmetric clone?)");
      return false;
    default :
      ROS_INFO("ERROR: Invalid return code from doPnP");
      return false;
  }
}

int Map::getPointsForBA(std::vector<int> &kfIDs,
                        std::map<int,std::map<int,int> > &points_for_ba)
{
  //Output: points_for_ba[ID] is a map that maps keyframe IDs of keyframes seeing it to
  //the index of the point in the keyframe

  //First take all points seen by any of the keyframes in kfIDs
  int i, nobs, ncam, n_kf_seeing_pt;
  std::map<int,std::map<int,int> >::iterator it;
  std::map<int,int>::iterator it2;
  ncam = kfIDs.size();
  for (i = 0; i < ncam; ++i)
    keyframes[kfIDs[i]]->getPointsSeen(points_for_ba);
  ROS_INFO("number of points seen = %lu",points_for_ba.size());

  //Remove points seen by only one of the keyframes in kfIDs, and count obs
  nobs = 0;
  for (it = points_for_ba.begin(); it != points_for_ba.end(); )
  {
    n_kf_seeing_pt = it->second.size();
    for (it2 = it->second.begin(); it2 != it->second.end();it2++)
    {
      //ROS_INFO("Keyframe %d sees point %d at index %d",it2->first,it->first,it2->second);
    }
    if (n_kf_seeing_pt < 2)
      points_for_ba.erase(it++);
    else
    {
      nobs += n_kf_seeing_pt;
      ++it;
    }
  }
  return nobs;
}

int Map::doPnP(const Frame& current_frame, ucl_drone::Pose3D& PnP_pose, int& n_inliers, double& fraction_FOV_without_inliers)
{
  std::vector<cv::Point3f> inliers_map_matching_points;
  std::vector<cv::Point2f> inliers_frame_matching_points;
  int result = matchWithFrame(current_frame, inliers_map_matching_points, inliers_frame_matching_points, fraction_FOV_without_inliers);
  n_inliers = inliers_map_matching_points.size();
  if (result < 0)
    return result;

  cv::Mat_<double> tcam, world2cam, drone2world, distCoeffs;
  distCoeffs = (cv::Mat_< double >(1, 5) << 0, 0, 0, 0, 0);

  // solve with PnP n>3
  cv::solvePnP(inliers_map_matching_points, inliers_frame_matching_points,
               camera.get_K(), distCoeffs, rvec, tvec, true, CV_EPNP);

  cv::Rodrigues(rvec, world2cam);
  if (fabs(determinant(world2cam)) - 1 > 1e-07)
    return -5;

  //front camera:
  cv::Mat cam2drone = camera.get_R();

  tcam = -world2cam.t() * tvec;
  PnP_pose.x = tcam(0);
  PnP_pose.y = tcam(1);
  PnP_pose.z = tcam(2);

  if (abs(PnP_pose.z - current_frame.pose.z) > 0.8)
    return -6;

  cv::Mat_<double> cam2world = world2cam.t();
  cv::Mat_<double> drone2cam = cam2drone.t();
  drone2world = cam2world * drone2cam;

  tf::Matrix3x3(drone2world(0, 0), drone2world(0, 1), drone2world(0, 2),
  drone2world(1, 0), drone2world(1, 1), drone2world(1, 2),
  drone2world(2, 0), drone2world(2, 1), drone2world(2, 2))
  .getRPY(PnP_pose.rotX, PnP_pose.rotY, PnP_pose.rotZ);

  PnP_pose.xvel    = 0.0;
  PnP_pose.yvel    = 0.0;
  PnP_pose.zvel    = 0.0;
  PnP_pose.rotXvel = 0.0;
  PnP_pose.rotYvel = 0.0;
  PnP_pose.rotZvel = 0.0;

  PnP_pose.header.stamp = current_frame.pose.header.stamp;  // needed for rqt_plot
  return 1;
}


bool customLess(std::vector< int > a, std::vector< int > b)
{
  return a[1] > b[1];
}

bool Map::keyframeNeeded(bool manual_pose_available, int n_inliers, double fraction_FOV_without_inliers, ucl_drone::Pose3D& current_pose)
{
  if (keyframes.size()==0)                        return true;
  if (manual_keyframes)                           return false;
  if (only_init && isInitialized())               return false;
  if (!isInitialized() && !manual_pose_available) return false;
  if (is_adjusting_bundle)                        return false;

  ros::Duration time_last_kf = ros::Time::now() - last_new_keyframe; //time elapsed
  double dist_last_kf = poseDistance(current_pose, keyframes.rbegin()->second->pose);
  double angle_dist = abs(current_pose.rotZ - keyframes.rbegin()->second->pose.rotZ);
  while (angle_dist >  PI) angle_dist -= 2*PI;
  while (angle_dist < -PI) angle_dist += 2*PI;

  if (fraction_FOV_without_inliers < 0.2)        return false;
  if (dist_last_kf < min_dist)                   return false;
  if (time_last_kf < ros::Duration(min_time))    return false;
  if (fraction_FOV_without_inliers > FOV_thresh) return true;
  if (dist_last_kf > dist_thresh)                return true;
  if (n_inliers < inliers_thresh)                return true;
  if (time_last_kf > ros::Duration(time_thresh)) return true;
  return false;
}

void cloud_debug(pcl::PointCloud< pcl::PointXYZ >::ConstPtr cloud)
{
  for (size_t i = 0; i < cloud->points.size(); ++i)
  {
    ROS_DEBUG("points[%lu] = (%f, %f, %f)", i, cloud->points[i].x, cloud->points[i].y, cloud->points[i].z);
  }
}

int Map::matchWithFrame(const Frame& frame, std::vector<cv::Point3f>& inliers_map_matching_points,
                std::vector<cv::Point2f>& inliers_frame_matching_points, double& fraction_FOV_without_inliers)
{
  if (frame.descriptors.rows == 0) return -1;
  if (descriptors.rows == 0)       return -2;
  double minx = frame.image.width;
  double miny = frame.image.height;
  double maxx = 0;
  double maxy = 0;
  std::vector<cv::Point3f> map_matching_points;
  std::vector<cv::Point2f> frame_matching_points;
  std::vector<int> map_indices, frame_indices, inliers;
  std::map<int,Landmark*>::iterator it;
  pcl::PointXYZ pcl_point;
  matchDescriptors(descriptors, frame.descriptors, map_indices, frame_indices, DIST_THRESHOLD,-1);
  if (map_indices.size() < threshold_lost)
    return -3;
  cv::Point2f img_pt;
  for (unsigned k = 0; k < map_indices.size(); k++)
  {
    pcl_point = this->cloud->points[map_indices[k]];
    cv::Point3f map_point(pcl_point.x,pcl_point.y,pcl_point.z);
    img_pt = frame.img_points[frame_indices[k]];
    map_matching_points.push_back(map_point);
    frame_matching_points.push_back(img_pt);
    if (img_pt.x < minx) minx = img_pt.x;
    if (img_pt.x > maxx) maxx = img_pt.x;
    if (img_pt.y < miny) miny = img_pt.y;
    if (img_pt.y > maxy) maxy = img_pt.y;
  }
  minx /= (double)frame.image.width;  miny /= (double)frame.image.height;
  maxx /= (double)frame.image.width;  maxy /= (double)frame.image.height;
  fraction_FOV_without_inliers = std::max(std::max(minx,1-maxx),std::max(miny,1-maxy));
  cv::Mat distCoeffs = (cv::Mat_< double >(1, 5) << 0, 0, 0, 0, 0);
  cv::solvePnPRansac(map_matching_points, frame_matching_points, camera.get_K(), distCoeffs, rvec, tvec,
                     true, 2500, 2, 100, inliers, CV_P3P);  // or: CV_EPNP and CV_ITERATIVE
  if (inliers.size() < threshold_lost)
    return -4;

  for (int j = 0; j < inliers.size(); j++)
  {
    int i = inliers[j];
    inliers_map_matching_points.push_back(map_matching_points[i]);
    inliers_frame_matching_points.push_back(frame_matching_points[i]);
  }
  std::sort(inliers.begin(),inliers.end());
  std::sort(map_indices.begin(),map_indices.end());
  std::map<int,Landmark*>::iterator landmarks_it = landmarks.begin();
  std::vector<int> points_to_remove;
  int next_inlier = 0;
  for (int k = 0; k<map_indices.size();k++)
  {
    if (k==0) std::advance(landmarks_it,map_indices[k]);
    else      std::advance(landmarks_it,map_indices[k]-map_indices[k-1]);
    if (k==inliers[next_inlier])
    {
      next_inlier++;
      landmarks_it->second->times_inlier++;
    }
    else
    {
      landmarks_it->second->times_outlier++;
    }
  }
  return 1;
}

void Map::doBundleAdjustment(std::vector<int> kfIDs, bool is_global)
{
  is_adjusting_bundle = true;
  int ncam, npt, nobs, i, j, k;
  std::map<int,std::map<int,int> > points_for_ba;
  std::map<int,std::map<int,int> >::iterator points_it;
  std::map<int,int>::iterator inner_it;

  //Remove unusable keyframes
  std::vector<int>::iterator it;
  for (it = kfIDs.begin(); it!=kfIDs.end(); )
    if (keyframes.find(*it) == keyframes.end())
      kfIDs.erase(it++);
    else if (keyframes[*it]->n_mapped_pts < 4)//arbitrary...
      kfIDs.erase(it++);
    else
      ++it;

  //Get points to adjust (in a map)
  nobs = getPointsForBA(kfIDs, points_for_ba);
  ROS_INFO("%lu points for BA",points_for_ba.size());
  ncam = kfIDs.size();
  npt  = points_for_ba.size();

  ucl_drone::BundleMsg::Ptr msg(new ucl_drone::BundleMsg);

  msg->is_global        = is_global;
  msg->num_keyframes    = ncam;
  msg->num_points       = npt;
  msg->num_observations = nobs;
  msg->observations.resize(nobs);
  msg->points.resize(npt);
  msg->points_ID.resize(npt);
  k = 0;
  std::map<int,int> this_point;

  points_it = points_for_ba.begin();
  for (i = 0; i < npt; ++i)
  {
    int ptID   = points_it->first;
    this_point = points_it->second;
    ROS_DEBUG("observations of point %d :",ptID);
    for (inner_it = this_point.begin(); inner_it != this_point.end(); ++inner_it)
    {
      int kfID = inner_it->first; //This is the ID
      int local_idx = inner_it->second;
      ROS_DEBUG("writing observation in message. kfID = %d; ptID = %d, idx in kf = %d",kfID,ptID,local_idx);
      msg->observations[k].kfID = kfID;
      msg->observations[k].ptID = ptID;
      msg->observations[k].x = keyframes[kfID]->img_points[local_idx].x;
      msg->observations[k].y = keyframes[kfID]->img_points[local_idx].y;
      k++;
    }
    ROS_DEBUG("done with observations of point %d",ptID);
    msg->points_ID[i] = ptID;
    msg->points[i].x = landmarks[ptID]->coordinates.x;
    msg->points[i].y = landmarks[ptID]->coordinates.y;
    msg->points[i].z = landmarks[ptID]->coordinates.z;
    ROS_DEBUG("done with point %d",ptID);
    ++points_it;
  }
  msg->cameras.resize(3*ncam);
  msg->poses.resize(ncam);
  msg->ref_poses.resize(ncam);
  msg->fixed_cams.resize(ncam);
  msg->keyframes_ID.resize(ncam);
  for (i = 0; i < ncam; ++i) {
    msg->cameras[3*i+0] = keyframes[kfIDs[i]]->camera->roll;
    msg->cameras[3*i+1] = keyframes[kfIDs[i]]->camera->pitch;
    msg->cameras[3*i+2] = keyframes[kfIDs[i]]->camera->yaw;
    msg->poses[i]       = keyframes[kfIDs[i]]->pose;
    msg->ref_poses[i]   = keyframes[kfIDs[i]]->ref_pose;
    msg->keyframes_ID[i] = kfIDs[i];
  }
  if (points_for_ba.size()==0)
    ROS_WARN("Warning: there are no matching points to do Bundle Adjustment");
  else
    bundle_pub.publish(*msg);
}


void Map::updateBundle(const ucl_drone::BundleMsg::ConstPtr bundlePtr)
{
  int npt, ncam, i, kfID, ptID;
  int n_kf_seeing_this_pt;
  ucl_drone::Pose3D thispose, prevpose;
  bool converged, remove_point;
  std::vector<int> keyframes_to_adjust;
  converged     = bundlePtr->converged;
  npt           = bundlePtr->num_points;
  ncam          = bundlePtr->num_keyframes;
  for (i = 0; i < ncam; ++i)
  {
    thispose = bundlePtr->poses[i];
    kfID     = bundlePtr->keyframes_ID[i];
    if (i==ncam-1 && poseDistance(prevpose,thispose)<0.05)
    {
      ROS_INFO("removing keyframe %d because it is too close to the previous one",kfID);
    }
    else
    {
      ROS_INFO("Updating keyframe %d",kfID);
      keyframes_to_adjust.push_back(kfID);
      keyframes[kfID]->pose = thispose;
      prevpose = thispose;
    }
  }
  //cost_of_point = for each point that was bundle adjusted, the cost divided by the number of keyframes seeing it that were bundle adjusted
  int pts_removed = 0;
  for (i = 0; i < npt; ++i)
  {
    ptID = bundlePtr->points_ID[i];
    n_kf_seeing_this_pt = landmarks[ptID]->keyframes_seeing.size();
    bool remove_this_point = outlier_threshold > 0 && bundlePtr->cost_of_point[i] > outlier_threshold;


    std::set<int>::iterator it;
    for (it = landmarks[ptID]->keyframes_seeing.begin();it != landmarks[ptID]->keyframes_seeing.end(); ++it)
    {
      if (!pointIsVisible(*keyframes[*it], landmarks[ptID]->coordinates,0)) remove_this_point = true;
    }

    if (remove_this_point && n_kf_seeing_this_pt == 2)
    {
      removePoint(ptID);
      pts_removed++;
    }
    else if (remove_this_point && n_kf_seeing_this_pt > 2)
    {
      //remove last observation of this point
      int kfID = *(landmarks[ptID]->keyframes_seeing.rbegin());
      int pt_idx_kf = keyframes[kfID]->point_indices[ptID];
      landmarks[ptID]->setAsUnseenBy(kfID);
      keyframes[kfID]->point_IDs[pt_idx_kf] = -2;
      keyframes[kfID]->point_indices.erase(ptID);
      keyframes[kfID]->n_mapped_pts--;
    }
    else
    {
      updatePoint(ptID,cv::Point3d(bundlePtr->points[i].x,bundlePtr->points[i].y,bundlePtr->points[i].z));
    }
  }
  ROS_INFO("Removed %d points",pts_removed);

  BA_times.push_back(bundlePtr->time_taken);
  BA_num_iter.push_back(bundlePtr->num_iter);

  bool do_global_BA = n_inliers_moving_avg > 70 && kf_since_last_global_BA > freq_global_ba && keyframes.size() > n_kf_local_ba;
  publishBenchmarkInfo();
  do_global_BA = false; //temp
  if (do_global_BA)
  {
    std::vector<int> all_kf_IDs;
    for(std::map<int,Keyframe*>::iterator it = keyframes.begin();it!=keyframes.end();++it)
    {
      all_kf_IDs.push_back(it->first);
    }
    doBundleAdjustment(all_kf_IDs,true);
    kf_since_last_global_BA = 0;
  }
  else
  {
    is_adjusting_bundle = false;
    last_new_keyframe   = ros::Time::now();
    kf_since_last_global_BA++;
  }
}

void Map::setManualPose(const ucl_drone::Pose3D& manual_pose)
{
  this->manual_pose = manual_pose;
  this->manual_pose_available = true;
}


void Map::publishBenchmarkInfo()
{
  ucl_drone::BenchmarkInfoMsg::Ptr msg(new ucl_drone::BenchmarkInfoMsg);

  msg->pts_map = landmarks.size();
  msg->keyframes_pose.resize(keyframes.size());
  msg->keyframes_ID.resize(keyframes.size());
  msg->n_pts_keyframe.resize(keyframes.size());
  msg->n_mapped_pts_keyframe.resize(keyframes.size());
  std::map<int,Keyframe*>::iterator it;
  int i = 0;
  for (it = keyframes.begin(); it!=keyframes.end(); ++it)
  {
    msg->keyframes_ID[i]          = it->first;
    msg->keyframes_pose[i]        = it->second->pose;
    msg->n_pts_keyframe[i]        = it->second->npts;
    msg->n_mapped_pts_keyframe[i] = it->second->n_mapped_pts;
    i++;
  }

  msg->BA_times    = BA_times.back();
  msg->BA_num_iter = BA_num_iter.back();

  benchmark_pub.publish(*msg);
}

void Map::print_info()
{
  print_landmarks();
  print_keyframes();
}

void Map::print_landmarks()
{
  int npts = landmarks.size();
  ROS_INFO("printing all (%d) landmarks:",npts);
  std::map<int,Landmark*>::iterator it;
  for (it = landmarks.begin(); it != landmarks.end();++it)
  {
    it->second->print();
    std::cout<<"                                  At indices: ";
    std::set<int>::iterator it2;
    for(it2 = it->second->keyframes_seeing.begin();it2!=it->second->keyframes_seeing.end();++it2)
    it2==it->second->keyframes_seeing.begin() ? std::cout<<keyframes[*it2]->point_indices[it->first] : std::cout<<", "<<keyframes[*it2]->point_indices[it->first];
    std::cout << std::endl;
  }
}

void Map::print_keyframes()
{
  int nkf = keyframes.size();
  ROS_INFO("printing all (%d) keyframes:",nkf);
  std::map<int,Keyframe*>::iterator it;
  for (it = keyframes.begin(); it != keyframes.end();++it)
  {
    it->second->print();
  }
}

void Map::print_benchmark_info()
{
  ROS_INFO("             == BENCHMARK INFO ==");
  ROS_INFO("  kf1     |   kf2     |   kf3     |   kf4");
  if (keyframes.size() == 1)
  {
    ROS_INFO("x = % 4.2f", keyframes[0]->pose.x);
    ROS_INFO("y = % 4.2f", keyframes[0]->pose.y);
    ROS_INFO("z = % 4.2f", keyframes[0]->pose.z);
    ROS_INFO("r = % 5.1f", keyframes[0]->pose.rotX*180/PI);
    ROS_INFO("p = % 5.1f", keyframes[0]->pose.rotY*180/PI);
    ROS_INFO("y = % 5.1f", keyframes[0]->pose.rotZ*180/PI);
  }
  if (keyframes.size() == 2)
  {
    ROS_INFO("x = % 4.2f | x = % 4.2f", keyframes[0]->pose.x          ,keyframes[1]->pose.x          );
    ROS_INFO("y = % 4.2f | y = % 4.2f", keyframes[0]->pose.y          ,keyframes[1]->pose.y          );
    ROS_INFO("z = % 4.2f | z = % 4.2f", keyframes[0]->pose.z          ,keyframes[1]->pose.z          );
    ROS_INFO("r = % 5.1f | r = % 5.1f", keyframes[0]->pose.rotX*180/PI,keyframes[1]->pose.rotX*180/PI);
    ROS_INFO("p = % 5.1f | p = % 5.1f", keyframes[0]->pose.rotY*180/PI,keyframes[1]->pose.rotY*180/PI);
    ROS_INFO("y = % 5.1f | y = % 5.1f", keyframes[0]->pose.rotZ*180/PI,keyframes[1]->pose.rotZ*180/PI);
  }
  if (keyframes.size() == 3)
  {
    ROS_INFO("x = % 4.2f | x = % 4.2f | x = % 4.2f",
    keyframes[0]->pose.x          , keyframes[1]->pose.x          , keyframes[2]->pose.x          );
    ROS_INFO("y = % 4.2f | y = % 4.2f | y = % 4.2f",
    keyframes[0]->pose.y          , keyframes[1]->pose.y          , keyframes[2]->pose.y          );
    ROS_INFO("z = % 4.2f | z = % 4.2f | z = % 4.2f",
    keyframes[0]->pose.z          , keyframes[1]->pose.z          , keyframes[2]->pose.z          );
    ROS_INFO("r = % 5.1f | r = % 5.1f | r = % 5.1f",
    keyframes[0]->pose.rotX*180/PI, keyframes[1]->pose.rotX*180/PI, keyframes[2]->pose.rotX*180/PI);
    ROS_INFO("p = % 5.1f | p = % 5.1f | p = % 5.1f",
    keyframes[0]->pose.rotY*180/PI, keyframes[1]->pose.rotY*180/PI, keyframes[2]->pose.rotY*180/PI);
    ROS_INFO("y = % 5.1f | y = % 5.1f | y = % 5.1f",
    keyframes[0]->pose.rotZ*180/PI, keyframes[1]->pose.rotZ*180/PI, keyframes[2]->pose.rotZ*180/PI);
  }
  if (keyframes.size() == 4)
  {
    ROS_INFO("x = % 4.2f | x = % 4.2f | x = % 4.2f | x = % 4.2f",
    keyframes[0]->pose.x          ,keyframes[1]->pose.x          ,keyframes[2]->pose.x          ,keyframes[3]->pose.x          );
    ROS_INFO("y = % 4.2f | y = % 4.2f | y = % 4.2f | y = % 4.2f",
    keyframes[0]->pose.y          ,keyframes[1]->pose.y          ,keyframes[2]->pose.y          ,keyframes[3]->pose.y          );
    ROS_INFO("z = % 4.2f | z = % 4.2f | z = % 4.2f | z = % 4.2f",
    keyframes[0]->pose.z          ,keyframes[1]->pose.z          ,keyframes[2]->pose.z          ,keyframes[3]->pose.z          );
    ROS_INFO("r = % 5.1f | r = % 5.1f | r = % 5.1f | r = % 5.1f",
    keyframes[0]->pose.rotX*180/PI,keyframes[1]->pose.rotX*180/PI,keyframes[2]->pose.rotX*180/PI,keyframes[3]->pose.rotX*180/PI);
    ROS_INFO("p = % 5.1f | p = % 5.1f | p = % 5.1f | p = % 5.1f",
    keyframes[0]->pose.rotY*180/PI,keyframes[1]->pose.rotY*180/PI,keyframes[2]->pose.rotY*180/PI,keyframes[3]->pose.rotY*180/PI);
    ROS_INFO("y = % 5.1f | y = % 5.1f | y = % 5.1f | y = % 5.1f",
    keyframes[0]->pose.rotZ*180/PI,keyframes[1]->pose.rotZ*180/PI,keyframes[2]->pose.rotZ*180/PI,keyframes[3]->pose.rotZ*180/PI);
  }

  if (!BA_times.empty())
  {
    ROS_INFO("ba_time = %f", BA_times.back());
  }

  int tot_obs = 0;
  std::map<int,Keyframe*>::iterator it_k;
  for(it_k = keyframes.begin(); it_k!=keyframes.end();++it_k)
    tot_obs += it_k->second->n_mapped_pts;
  int npts = landmarks.size();
  double avg_obs = (double)tot_obs/(double)npts;
  ROS_INFO("npts = %d ; average number of keyframes per landmark = %f",npts,avg_obs);
}
