#include <boris_drone/map/map.h>

int Map::point_ID_counter = 0;

Map::Map() {}

Map::Map(ros::NodeHandle* nh) : cloud(new pcl::PointCloud< pcl::PointXYZ >())
{
  cv::initModule_nonfree();  // initialize OpenCV SIFT and SURF

  nh = nh;
  bundle_channel = nh->resolveName("bundle");
  bundle_pub     = nh->advertise<boris_drone::BundleMsg>(bundle_channel, 1);

  benchmark_channel = nh->resolveName("benchmark");
  benchmark_pub     = nh->advertise<boris_drone::BenchmarkInfoMsg>(benchmark_channel, 1);

  //Get some parameters from launch file
  ros::param::get("~2D_noise", use_2D_noise);
  ros::param::get("~3D_noise", use_3D_noise);
  ros::param::get("~threshold_kf_match", threshold_kf_match);
  ros::param::get("~max_matches", max_matches);
  ros::param::get("~no_bundle_adjustment", no_bundle_adjustment);
  ros::param::get("~double_ba", double_ba);
  ros::param::get("~dlt_triangulation", dlt_triangulation);
  ros::param::get("~midpoint_triangulation", midpoint_triangulation);
  ros::param::get("~only_init", only_init);
  ros::param::get("~rpt2", rpt2);
  ros::param::get("~rpt3", rpt3);
  ros::param::get("~rpt4", rpt4);
  ros::param::get("~remove_cst", remove_cst);
  ros::param::get("~remove_coeff", remove_coeff);
  ros::param::get("~manual_keyframes", manual_keyframes);
  ros::param::get("~benchmark", benchmark);
  ROS_INFO("init map");


  ROS_INFO("use 2D noise ? %s. Use 3D noise? %s",use_2D_noise?"true":"false",use_3D_noise?"true":"false");
  // define some threshold used later
  // better if defined in the launch file
  frame_counter = 0;
  is_adjusting_bundle     = false;
  second_keyframe_pending = false;
  init = false;
  triangtime = 0.0;
  nptstriang =0;
  last_removal = ros::Time::now();

  camera = Camera(true);
  // get camera parameters in launch file
  if (!Read::CamMatrixParams("cam_matrix"))
    ROS_ERROR("cam_matrix not properly transmitted");
  if (!Read::ImgSizeParams("img_size"))
    ROS_ERROR("img_size not properly transmitted");

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
  for(it_k = keyframes.begin(); it_k!=keyframes.end();++it_k)
    delete it_k->second;
  for(it_l = landmarks.begin(); it_l!=landmarks.end();++it_l)
    delete it_l->second;
  keyframes.clear();
  landmarks.clear();
  cloud = boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ> >(new pcl::PointCloud<pcl::PointXYZ>);
  tvec = cv::Mat::zeros(3, 1, CV_64FC1);
  rvec = cv::Mat::zeros(3, 1, CV_64FC1);
}

bool Map::isInitialized(){  return (keyframes.size() > 3);}

int Map::addPoint(cv::Point3d& coordinates, cv::Mat descriptor)
{
  pcl::PointXYZ new_point;
  new_point.x = coordinates.x;
  new_point.y = coordinates.y;
  new_point.z = coordinates.z;
  cloud->points.push_back(new_point);
  descriptors.push_back(descriptor);
  Landmark* new_landmark = new Landmark(coordinates, descriptor);
  landmarks[new_landmark->ID] = new_landmark;
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
  //ROS_INFO("removing point %d",ptID);
  bool keyframe_is_dead;
  std::map<int,Landmark*>::iterator it = landmarks.find(ptID);
  if(it==landmarks.end())
  {
    ROS_INFO("Trying to remove point %d but it doesn't exist",ptID);
    return;
  }
  int idx = std::distance(landmarks.begin(),it);
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
  if (idx > 0)      // Copy everything above that one idx.
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
  std::map<int,int>::iterator it2; //point_indices;
  for (it2 = kf->point_indices.begin(); it2!=kf->point_indices.end();++it2)
  {
    ROS_DEBUG("removing point %d from keyframe %d",it2->first,kfID);
    point_is_dead = landmarks[it2->first]->setAsUnseenBy(kfID);
    if (point_is_dead)
      removePoint(it2->first);
  }
  delete kf;
  keyframes.erase(kfID);
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
  int i, nmatch, ptID, ptID_kf0, ptID_kf1;
  cv::Point3d point3D;
  std::vector<int> idx_kf0, idx_kf1;
  matchDescriptors(kf0->descriptors, kf1->descriptors, idx_kf0, idx_kf1, threshold_kf_match, max_matches);
  nmatch = idx_kf0.size();
  ROS_INFO("%d matches over threshold",nmatch);
  TIC(triang)
  for (i = 0; i<nmatch; i++)
  {
    ROS_DEBUG("indices of match %d are %d and %d",i,idx_kf0[i],idx_kf1[i]);
    ptID_kf0 = kf0->point_IDs[idx_kf0[i]];
    ptID_kf1 = kf1->point_IDs[idx_kf1[i]];
    ROS_DEBUG("IDs of match %d are %d and %d",i,ptID_kf0,ptID_kf1);
    if ((ptID_kf0==-1) && (ptID_kf1==-1))
    {
      nptstriang++;
      if (dlt_triangulation)
        triangulate_dlt(point3D, kf0, kf1, idx_kf0[i], idx_kf1[i]);
      else if (midpoint_triangulation)
        triangulate_midpoint(point3D, kf0, kf1, idx_kf0[i], idx_kf1[i]);
      else
        triangulate(point3D, kf0, kf1, idx_kf0[i], idx_kf1[i]);
      ptID = addPoint(point3D, kf0->descriptors.rowRange(idx_kf0[i],idx_kf0[i]+1));
      setPointAsSeen(ptID, kf0->ID, idx_kf0[i]);
      setPointAsSeen(ptID, kf1->ID, idx_kf1[i]);
    }
    else if ((ptID_kf0==-1)&&(ptID_kf1!=-2))
    {
      setPointAsSeen(ptID_kf1, kf0->ID, idx_kf0[i]);
    }
    else if ((ptID_kf1==-1)&&(ptID_kf0!=-2))
    {
      setPointAsSeen(ptID_kf0, kf1->ID, idx_kf1[i]);
    }
  }
  triangtime += TOC(triang);
  ROS_INFO("finished matching keyframe %d with keyframe %d. There are %d matching points",kf0->ID, kf1->ID, nmatch);
}

void Map::newKeyframe(const Frame& frame)
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
  if (keyframes.size() < 5)
    first_kf_to_adjust = keyframes.begin();
  else
    ++first_kf_to_adjust;
  if (keyframes.size() < 2)
    return;
  std::vector<int> keyframes_to_adjust;
  std::map<int,Keyframe*>::iterator it;
  //for (it = first_kf_to_adjust; it!=keyframes.end(); ++it)
  for (it = keyframes.begin(); it!=keyframes.end(); ++it)
  {
    if (it->first != new_keyframe->ID)
    {
      ROS_INFO("Matching keyframes %d and %d",it->first,new_keyframe->ID);
      //print_landmarks();
      matchKeyframes(new_keyframe, it->second);
      //print_landmarks();
    }
    keyframes_to_adjust.push_back(it->first); //add all kfs
  }

  ROS_INFO("\t Map now has %lu points",this->cloud->points.size());
  if (no_bundle_adjustment)
  {
    ROS_INFO("total triang time = %f for %d points",triangtime,nptstriang);
    return;
  }
  doBundleAdjustment(keyframes_to_adjust,true);
}

int Map::cleanMap()
{
  std::map<int,Landmark*>::iterator it;
  int nptsremoved = 0;
  for (it = landmarks.begin(); it != landmarks.end(); )
  {
    if((abs(it->second->coordinates.x)>10)||(abs(it->second->coordinates.y)>10)||(abs(it->second->coordinates.z)>10))
    {
      removePoint((it++)->first);
      nptsremoved++;
    }
    else
      ++it;
  }
  return nptsremoved;
}

bool Map::processFrame(Frame& frame, boris_drone::Pose3D& PnP_pose, bool manual_pose_received)
{
  ros::Duration five_minutes(300.0);

  if (init==false)
  {
    tStart = ros::Time::now();
    init = true;
  }
  else if (ros::Time::now() - tStart > five_minutes)
  {
    tStart = ros::Time::now();
  }

  int n_inliers;
  int PnP_result = doPnP(frame, PnP_pose, n_inliers);
  int n_keyframes = keyframes.size();
  if (keyframeNeeded(manual_pose_received, n_inliers))
  {
    if ((use_2D_noise||use_3D_noise)&&n_keyframes!=0)
    {
      frame.pose.x    += (0.300)*(double)(n_keyframes==1||n_keyframes==2)*use_2D_noise;
      frame.pose.y    += (0.300)*(double)(n_keyframes==2||n_keyframes==3)*use_2D_noise;
      frame.pose.rotZ += (PI/10)*(double)(n_keyframes==3||n_keyframes==1)*use_2D_noise;
      frame.pose.z    += (0.050)*(double)(n_keyframes==1)*use_3D_noise;
      frame.pose.rotX += (PI/30)*(double)(n_keyframes==2)*use_3D_noise;
      frame.pose.rotY += (PI/30)*(double)(n_keyframes==3)*use_3D_noise;
    }
    else if ((PnP_result==1)&&(!benchmark))
    {
      ROS_INFO("new keyframe with pnp");
      frame.pose.x    = PnP_pose.x;
      frame.pose.y    = PnP_pose.y;
      frame.pose.rotZ = PnP_pose.rotZ;
    }
    newKeyframe(frame);
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

int Map::doPnP(const Frame& current_frame, boris_drone::Pose3D& PnP_pose, int& n_inliers)
{
  std::vector<cv::Point3f> inliers_map_matching_points;
  std::vector<cv::Point2f> inliers_frame_matching_points;
  int result = matchWithFrame(current_frame, inliers_map_matching_points, inliers_frame_matching_points);
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

bool Map::keyframeNeeded(bool manual_pose_received, int n_inliers)
{
  if (keyframes.size()==0)          return true;
  if (manual_keyframes)             return manual_pose_received;
  if (only_init && isInitialized()) return false; //(for benchmark of initialization)
  if (!isInitialized())             return manual_pose_received;
  if (is_adjusting_bundle)          return false;
//if (second_keyframe_pending)
//{
////second_keyframe_pending = false;
////queue_of_frames.clear();
////frame_counter = 0;
////return true;
//}
  if (n_inliers>threshold_inliers_new_keyframe) return false;
  return (ros::Time::now() - last_new_keyframe) > ros::Duration(5.0);
}

void cloud_debug(pcl::PointCloud< pcl::PointXYZ >::ConstPtr cloud)
{
  for (size_t i = 0; i < cloud->points.size(); ++i)
  {
    ROS_DEBUG("points[%lu] = (%f, %f, %f)", i, cloud->points[i].x, cloud->points[i].y,
              cloud->points[i].z);
  }
}

void Map::getDescriptors(cv::Mat &map_descriptors)
{
  std::map<int,Landmark*>::iterator it;
  for (it=landmarks.begin();it!=landmarks.end();++it)
  {
    map_descriptors.push_back(it->second->descriptor);
  }
}

int Map::matchWithFrame(const Frame& frame, std::vector<cv::Point3f>& inliers_map_matching_points,
                                            std::vector<cv::Point2f>& inliers_frame_matching_points)
{
  if (frame.descriptors.rows == 0)
    return -1;
  if (descriptors.rows == 0)
    return -2;

  std::vector<cv::Point3f> map_matching_points;
  std::vector<cv::Point2f> frame_matching_points;
  std::vector<int> map_indices, frame_indices, inliers;
  std::map<int,Landmark*>::iterator it;
  pcl::PointXYZ pcl_point;
  matchDescriptors(descriptors, frame.descriptors, map_indices, frame_indices, DIST_THRESHOLD,-1);
  if (map_indices.size() < threshold_lost)
    return -3;

  for (unsigned k = 0; k < map_indices.size(); k++)
  {
    pcl_point = this->cloud->points[map_indices[k]];
    cv::Point3f map_point(pcl_point.x,pcl_point.y,pcl_point.z);
    map_matching_points.push_back(map_point);
    frame_matching_points.push_back(frame.img_points[frame_indices[k]]);
  }

  cv::Mat distCoeffs = (cv::Mat_< double >(1, 5) << 0, 0, 0, 0, 0);
  cv::solvePnPRansac(map_matching_points, frame_matching_points, camera.get_K(), distCoeffs, rvec, tvec,
                     true, 2500, 2, 2 * threshold_new_keyframe, inliers, CV_P3P);  // or: CV_EPNP and CV_ITERATIVE
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

void Map::removeUnusedPoints()
{
  last_removal = ros::Time::now();
  std::map<int,Landmark*>::iterator it;
  for (it = landmarks.begin();it!=landmarks.end(); )
  {
    int n_out = it->second->times_outlier;
    int n_in  = it->second->times_inlier;
    if (n_out > remove_cst + remove_coeff*n_in)
      removePoint((it++)->first);
    else
      ++it;
  }
}

void Map::doBundleAdjustment(std::vector<int> kfIDs, bool is_first_pass)
{
  if (remove_coeff > 0) removeUnusedPoints();
  if (is_first_pass) is_adjusting_bundle = true;
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

  boris_drone::BundleMsg::Ptr msg(new boris_drone::BundleMsg);

  msg->is_first_pass    = is_first_pass;
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
      //ROS_DEBUG("Observation %d: Cam index = %d; Point index = %d",k,kfID,ptID);
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


void Map::updateBundle(const boris_drone::BundleMsg::ConstPtr bundlePtr)
{
  //print_info();
  int npt, ncam, i, kfID, ptID;
  int n_kf_pt;
  bool converged, is_first_pass, remove_point;
  std::vector<int> keyframes_to_adjust;
  converged     = bundlePtr->converged;
  is_first_pass = bundlePtr->is_first_pass;
  npt           = bundlePtr->num_points;
  ncam          = bundlePtr->num_keyframes;
  for (i = 0; i < ncam; ++i) {
    kfID = bundlePtr->keyframes_ID[i];
    keyframes_to_adjust.push_back(kfID);
    ROS_INFO("Updating keyframe %d",kfID);
    keyframes[kfID]->pose = bundlePtr->poses[i];
  }
  int pts_removed = 0;
  for (i = 0; i < npt; ++i)
  {
    ptID = bundlePtr->points_ID[i];
    n_kf_pt = landmarks[ptID]->keyframes_seeing.size();
    switch(n_kf_pt){
      case 2  :
        remove_point = rpt2 < 0 ? false : (bundlePtr->cost_of_point[i]>rpt2);
        break;
      case 3  :
        remove_point = rpt3 < 0 ? false : (bundlePtr->cost_of_point[i]>rpt3);
        break;
      case 4  :
        remove_point = rpt4 < 0 ? false : (bundlePtr->cost_of_point[i]>rpt4);
        break;
      default :
        remove_point = false;
    }
    if (remove_point)
    {
      removePoint(ptID);
      pts_removed++;
    }
    else
    {
      updatePoint(ptID,cv::Point3d(bundlePtr->points[i].x,bundlePtr->points[i].y,bundlePtr->points[i].z));
    }
  }
  int pts_removed_clean = cleanMap();
  ROS_INFO("Removed %d + %d points",pts_removed,pts_removed_clean);
  //print_info();
  if (is_first_pass&&double_ba)
  {
    BA_times_pass1.push_back(bundlePtr->time_taken);
    num_iter_pass1.push_back(bundlePtr->num_iter);
    doBundleAdjustment(keyframes_to_adjust,false);
  }
  else
  {
    BA_times_pass2.push_back(bundlePtr->time_taken);
    num_iter_pass2.push_back(bundlePtr->num_iter);
    is_adjusting_bundle = false;
    last_new_keyframe   = ros::Time::now();
    publishBenchmarkInfo();
    //print_landmarks();
  }
}

void Map::publishBenchmarkInfo()
{
  boris_drone::BenchmarkInfoMsg::Ptr msg(new boris_drone::BenchmarkInfoMsg);

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

  if (double_ba) msg->BA_times_pass1 = BA_times_pass1.back();
  else           msg->BA_times_pass1 = 0.0;
  if (double_ba) msg->num_iter_pass1 = num_iter_pass1.back();
  else           msg->num_iter_pass1   = 0;
  msg->BA_times_pass2 = BA_times_pass2.back();
  msg->num_iter_pass2 = num_iter_pass2.back();

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
