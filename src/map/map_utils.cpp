

#include <boris_drone/map/map_utils.h>

void matchDescriptors(const cv::Mat& descriptors1, const cv::Mat& descriptors2,
  std::vector<int>& matching_indices_1, std::vector<int>& matching_indices_2)
{
  cv::FlannBasedMatcher matcher;
  std::vector<cv::DMatch> simple_matches;
  matcher.match(descriptors1, descriptors2, simple_matches);
  std::vector<int> indices_of_the_matches;
  double thisdistance, otherdistance;
  size_t idx;
  // threshold test
  for (unsigned k = 0; k < simple_matches.size(); k++)
  {
    thisdistance = simple_matches[k].distance;
    if (thisdistance < DIST_THRESHOLD)
    {
      //Find if trainIdx has already been matched
      std::vector<int>::iterator it = find(matching_indices_2.begin(),
                         matching_indices_2.end(), simple_matches[k].trainIdx);
      if(it != matching_indices_2.end())
      {
        idx = it - matching_indices_2.begin();
        otherdistance = simple_matches[indices_of_the_matches[idx]].distance;
        //Find if this match is better than the old one, replace it
        if (thisdistance < otherdistance)
        {
          matching_indices_1[idx] = simple_matches[k].queryIdx;
        }
      }
      //If it hasn't been matched yet, add it
      else
      {
        matching_indices_1.push_back(simple_matches[k].queryIdx);
        matching_indices_2.push_back(simple_matches[k].trainIdx);
        indices_of_the_matches.push_back(k); //lol
      }
    }
  }
}


bool triangulate(cv::Point3d& pt_out, const cv::Point2d& pt1, const cv::Point2d& pt2,
                      const boris_drone::Pose3D& pose1, const boris_drone::Pose3D& pose2)
{
//in matlab this is triangulate4 lol
//http://www.iim.cs.tut.ac.jp/~kanatani/papers/sstriang.pdf
//(iterative method for higher order aproximation)
  /* get coordinates */
  cv::Mat cam2world1, cam2world2, origin1, origin2;
  cv::Mat T1, T2, K1, K2, cam0, cam1, F;
  cv::Mat pt1_h, pt2_h, u, P, x_hat, x1_hat, temp, x_tilde, x1_tilde;
  cv::Mat V, epsil, mult;
  double E, E0, f, den ;
  getCameraPositionMatrices(pose1, cam2world1, origin1, true);
  getCameraPositionMatrices(pose2, cam2world2, origin2, true);

  T1 = (cv::Mat_<double>(3, 4) << 1, 0, 0, -origin1.at<double>(0,0),
                                  0, 1, 0, -origin1.at<double>(1,0),
                                  0, 0, 1, -origin1.at<double>(2,0)
  );
  T2 = (cv::Mat_<double>(3, 4) << 1, 0, 0, -origin2.at<double>(0,0),
                                  0, 1, 0, -origin2.at<double>(1,0),
                                  0, 0, 1, -origin2.at<double>(2,0)
  );
  K2 = (cv::Mat_<double>(3, 3) << 529.1, 0    , 350.6,
                                  0,     529.1, 182.2,
                                  0,     0,     1
  );
  K1 = (cv::Mat_<double>(3, 3) << 529.1, 0    , 350.6,
                                  0,     529.1, 182.2,
                                  0,     0,     1
  );

  //These are the camera projection matrices:
  // If p is a world point in 3D in the world coordinates,
  // then cam0*p are the image coordinates of the world point
  cam0 = K1*cam2world1.t()*T1;
  cam1 = K2*cam2world2.t()*T2;
  F = (cv::Mat_<double>(3, 3)); // Fundamental Matrix3x3
  getFundamentalMatrix(cam0,cam1,F);

  pt1_h = (cv::Mat_<double>(3,1) << pt1.x, pt1.y, 1);
  pt2_h = (cv::Mat_<double>(3,1) << pt2.x, pt2.y, 1);

  u = (cv::Mat_<double>(9,1) << F.at<double>(0,0),F.at<double>(0,1),F.at<double>(0,2),
                                F.at<double>(1,0),F.at<double>(1,1),F.at<double>(1,2),
                                F.at<double>(2,0),F.at<double>(2,1),F.at<double>(2,2));
  P = (cv::Mat_<double>(3,3) << 1,0,0, 0,1,0, 0,0,0);

  f = 250;
  f = 1;

  x_hat  = pt1_h.clone();
  x1_hat = pt2_h.clone();

  x_tilde  = (cv::Mat_<double>(3,1) << 0,0,0);
  x1_tilde = (cv::Mat_<double>(3,1) << 0,0,0);

  int k = 0; E = 1; E0 = 0;
  while (abs(E - E0)> 0.001)
  {
    k = k+1;
    E0 = E;
    x_hat  = pt1_h - x_tilde;
    x1_hat = pt2_h - x1_tilde;
    getVandEpsil(V, epsil, x_hat, x1_hat, x_tilde, x1_tilde, f);
    temp = u.t()*V*u;
    den  = temp.at<double>(0,0);
    temp = u.t()*epsil;
    mult =  P*temp.at<double>(0,0)/den;
    x_tilde  = mult*F    *x1_hat;
    x1_tilde = mult*F.t()*x_hat ;
    E =  x_tilde.at<double>(0,0) *x_tilde.at<double>(0,0)  + x_tilde.at<double>(1,0) *x_tilde.at<double>(1,0)
       + x1_tilde.at<double>(0,0)*x1_tilde.at<double>(0,0) + x1_tilde.at<double>(1,0)*x1_tilde.at<double>(1,0);
  }
  cv::Point2d p1,p2;
  p1.x = x_hat.at<double>(0,0);
  p1.y = x_hat.at<double>(1,0);
  p2.x = x1_hat.at<double>(0,0);
  p2.y = x1_hat.at<double>(1,0);

  std::vector<cv::Point2d> cam0pnts;
  std::vector<cv::Point2d> cam1pnts;
  cam0pnts.push_back(p1);
  cam1pnts.push_back(p2);

  cv::Mat pnts3D(4,cam0pnts.size(),CV_64F);
  cv::triangulatePoints(cam0,cam1,cam0pnts,cam1pnts,pnts3D);

  pt_out.x = pnts3D.at<double>(0,0)/pnts3D.at<double>(3,0);
  pt_out.y = pnts3D.at<double>(1,0)/pnts3D.at<double>(3,0);
  pt_out.z = pnts3D.at<double>(2,0)/pnts3D.at<double>(3,0);

  return true;
}


inline void getVandEpsil(cv::Mat& V, cv::Mat& epsil, cv::Mat& x_hat, cv::Mat& x1_hat,
  cv::Mat& x_tilde, cv::Mat& x1_tilde, double f)
{
  double x,y,x1,y1,x2,y2,x12,y12,xt,x1t,yt,y1t;
  x   = x_hat.at<double>(0,0)   ;  y   = x_hat.at<double>(1,0)   ;
  x1  = x1_hat.at<double>(0,0)  ;  y1  = x1_hat.at<double>(1,0)  ;
  xt  = x_tilde.at<double>(0,0) ;  yt  = x_tilde.at<double>(1,0) ;
  x1t = x1_tilde.at<double>(0,0);  y1t = x1_tilde.at<double>(1,0);
  x2 = x*x; x12 = x1*x1;  y2 = y*y; y12 = y1*y1;

  V=(cv::Mat_<double>(9,9) <<
     x2+x12,x1*y1, f*x1,  x*y,   0,     0,   f*x,0,  0,
     x1*y1, x2+y12,f*y1,  0,     x*y,   0,   0,  f*x,0,
     f*x1,  f*y1,  f*f,   0,     0,     0,   0,  0,  0,
     x*y,   0,     0,     y2+x12,x1*y1, f*x1,f*y,0,  0,
     0,     x*y,   0,     x1*y1, y2+y12,f*y1,0,  f*y,0,
     0,     0,     0,     f*x1,  f*y1,  f*f, 0  ,0,  0,
     f*x,   0,     0,     f*y,   0,     0,   f*f,0,  0,
     0,     f*x,   0,     0,     f*y,   0,   0,  f*f,0,
     0,0,0,               0,0,0,             0,0,0);
  epsil = (cv::Mat_<double>(9,1) <<
           x*x1 + x1*xt + x*x1t,
           x*y1 + y1*xt + x*y1t,
           (x   + xt)*f        ,
           y*x1 + x1*yt + y*x1t,
           y*y1 + y1*yt + y*y1t,
           (y   + yt)*f        ,
           (x1  + x1t)*f       ,
           (y1  + y1t)*f       ,
           f*f                 );
}