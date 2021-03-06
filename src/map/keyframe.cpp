/*
 *  This file is part of ucl_drone 2017.
 *  For more information, please refer
 *  to the corresponding header file.
 *
 *  \author Boris Dehem
 *  \date 2017
 *
 */

#include <ucl_drone/map/keyframe.h>

int Keyframe::ID_counter = 0;

Keyframe::Keyframe() {}

Keyframe::Keyframe(const Frame& frame, Camera* cam)
{
  this->camera = cam;
  this->ID     = ID_counter++;
  this->pose   = frame.pose; //Rotation is to drone
  this->ref_pose     = pose;
  this->img_points   = frame.img_points;
  this->descriptors  = frame.descriptors;
  this->npts         = img_points.size();
  this->n_mapped_pts = 0;
  this->point_IDs.resize(npts,-1);
  ROS_INFO("Created keyframe %d. It has %d (unmatched) points",ID,npts);
}

Keyframe::~Keyframe() {}

void Keyframe::setAsSeeing(int ptID, int idx_in_kf)
{
  std::map<int,int>::iterator it = point_indices.find(ptID);
  if (it==point_indices.end())
  {
    point_IDs[idx_in_kf] = ptID;
    point_indices[ptID] = idx_in_kf;
    n_mapped_pts++;
  }
  else
    ROS_DEBUG("Trying to put kf %d as seeing pt %d, but it already sees it!",ID,ptID);
}

//Adds to the map of points to keyframes and index within them
void Keyframe::getPointsSeen(std::map<int,std::map<int,int> >& points)
{
  std::pair <int,int> this_observation;
  for (int i = 0; i<npts; i++)
  {
    ROS_DEBUG("keyframe %d at index %d sees point %d",ID,i,point_IDs[i]);
    if (point_IDs[i]>=0)
    {
      this_observation = std::make_pair(ID,i);
      points[point_IDs[i]].insert(this_observation);
    }
  }
}

bool Keyframe::removePoint(int ptID)
{
  std::map<int,int>::iterator it = point_indices.find(ptID);
  if (it==point_indices.end())
  {
    ROS_WARN("Tried to remove point %d from keyframe %d but point is not in keyframe",ptID,ID);
    return false;
  }
  point_IDs[it->second] = -2;
  point_indices.erase(it);
  n_mapped_pts--;
  return (n_mapped_pts<1);
}

void Keyframe::print()
{
  ROS_INFO("Keyframe ID = %d",ID);
  ROS_INFO("  Number of keypoints seen = %d",npts);
  ROS_INFO("  Number of landmarks seen = %d",n_mapped_pts);
  ROS_INFO("  Keypoints:");
  for (int i = 0; i < npts; i++)
  {
    switch (point_IDs[i]) {
      case -2:
        ROS_INFO("    Point %d has been removed from map",i);
        break;
      case -1:
        ROS_INFO("    Point %d is not mapped",i);
        break;
      default:
        ROS_INFO("    Point %d corresponds to landmark %d",i, point_IDs[i]);
    }
  }
}
