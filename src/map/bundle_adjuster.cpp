
// A minimal, self-contained bundle adjuster using Ceres, that reads
// files from University of Washington' Bundle Adjustment in the Large dataset:
// http://grail.cs.washington.edu/projects/bal
//
// This does not use the best configuration for solving; see the more involved
// bundle_adjuster.cc file for details.
#include <boris_drone/map/bundle_adjuster.h>

BALProblem::BALProblem(const boris_drone::BundleMsg::ConstPtr bundlePtr)
{
  bundleMsgPtr_     = bundlePtr;
  num_cameras_      = bundlePtr->num_cameras;
  num_points_       = bundlePtr->num_points;
  num_observations_ = bundlePtr->num_observations;
  num_parameters_   = 6 * num_cameras_ + 3 * num_points_;
  point_index_  = new int[num_observations_];
  camera_index_ = new int[num_observations_];
  fixed_cams_   = new bool[num_cameras_];
  observations_ = new double[2 * num_observations_];
  parameters_   = new double[num_parameters_];
  ref_poses_    = new double[6 * num_cameras_];

  //Mapping keyframe and point IDs to indices for the BA
  std::map<int,int> kfID_to_idx;
  std::map<int,int> ptID_to_idx;
  for(int i = 0; i < num_cameras_; ++i)
    kfID_to_idx[bundlePtr->keyframes_ID[i]] = i;
  for(int i = 0; i < num_points_ ; ++i)
    ptID_to_idx[bundlePtr->points_ID[i]] = i;

  for (int i = 0; i < num_observations_; ++i) {
    camera_index_[i]     = kfID_to_idx[bundlePtr->observations[i].kfID];
    point_index_[i]      = ptID_to_idx[bundlePtr->observations[i].ptID];
    observations_[2*i]   = bundlePtr->observations[i].x;
    observations_[2*i+1] = bundlePtr->observations[i].y;
  }
  for (int i = 0; i < num_cameras_; ++i) {
    parameters_[6*i + 0] = bundlePtr->cameras[i].x;
    parameters_[6*i + 1] = bundlePtr->cameras[i].y;
    parameters_[6*i + 2] = bundlePtr->cameras[i].z;
    parameters_[6*i + 3] = bundlePtr->cameras[i].rotX*180/PI;
    parameters_[6*i + 4] = bundlePtr->cameras[i].rotY*180/PI;
    parameters_[6*i + 5] = bundlePtr->cameras[i].rotZ*180/PI;
    ref_poses_[6*i + 0]  = bundlePtr->ref_poses[i].x;
    ref_poses_[6*i + 1]  = bundlePtr->ref_poses[i].y;
    ref_poses_[6*i + 2]  = bundlePtr->ref_poses[i].z;
    ref_poses_[6*i + 3]  = bundlePtr->ref_poses[i].rotX*180/PI;
    ref_poses_[6*i + 4]  = bundlePtr->ref_poses[i].rotY*180/PI;
    ref_poses_[6*i + 5]  = bundlePtr->ref_poses[i].rotZ*180/PI;
    fixed_cams_[i] = bundlePtr->fixed_cams[i];
  }
  for (int i = 0; i < num_points_; ++i) {
    parameters_[6*num_cameras_ + 3*i + 0] = bundlePtr->points[i].x;
    parameters_[6*num_cameras_ + 3*i + 1] = bundlePtr->points[i].y;
    parameters_[6*num_cameras_ + 3*i + 2] = bundlePtr->points[i].z;
  }
}
BALProblem::~BALProblem() {
  delete[] point_index_;
  delete[] camera_index_;
  delete[] observations_;
  delete[] parameters_;
  delete[] fixed_cams_;
  delete[] ref_poses_;
}
int BALProblem::num_observations()       const { return num_observations_;               }
const double* BALProblem::observations() const { return observations_;                   }
double* BALProblem::mutable_cameras()          { return parameters_;                     }
double* BALProblem::mutable_points()           { return parameters_  + 6 * num_cameras_; }
double* BALProblem::ref_pose(int i)            { return ref_poses_   + 6 * i;            }
double* BALProblem::mutable_camera(int i)      { return mutable_cameras()  + 6 * i;      }
double* BALProblem::mutable_point(int i)       { return mutable_points()   + 3 * i;      }
double* BALProblem::mutable_camera_for_observation(int i) { return mutable_camera(camera_index_[i]);}
double* BALProblem::mutable_point_for_observation(int i)  { return mutable_point(point_index_[i]);  }

// Templated pinhole camera model for used with Ceres.  The camera is
// parameterized using 6 parameters: 3 for rotation, 3 for translation,
struct SnavelyReprojectionError {
  SnavelyReprojectionError(double observed_x, double observed_y)
      : observed_x(observed_x), observed_y(observed_y) {}
  template <typename T>
  bool operator()(const T* const camera,
                  const T* const point,
                  T* residuals) const {
    T cam2world[9];              //rotation matrix
    T camera_axis_angles[3];     //angle-axis rotation
    T p_cam[3];
    T p[3];
    //Convert
    ceres::EulerAnglesToRotationMatrix(camera + 3, 3, cam2world);
    ceres::RotationMatrixToAngleAxis(cam2world, camera_axis_angles);
    //Translate point to get location from camera origin
    p_cam[0] = point[0] - camera[0];
    p_cam[1] = point[1] - camera[1];
    p_cam[2] = point[2] - camera[2];
    //Get rotated point. -1 because this is the rotation of cam2world (we need world2cam)
    ceres::AngleAxisRotatePoint(camera_axis_angles, p_cam, p);
    // TODO include fx,fy,cx,cy instead of hardcode
    //Remider:
    //K2 = (cv::Mat_<double>(3, 3) << 529.1, 0    , 350.6,
    //                                0,     529.1, 182.2,
    //                                0,     0,     1
    T predicted_x = 350.6 + (529.1*p[0] / p[2]);
    T predicted_y = 182.2 + (529.1*p[1] / p[2]);
    // The error is the difference between the predicted and observed position.
    residuals[0] = predicted_x - observed_x;
    residuals[1] = predicted_y - observed_y;
    return true;
  }
  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(const double observed_x,
                                     const double observed_y) {
    return (new ceres::AutoDiffCostFunction<SnavelyReprojectionError, 2, 6, 3>(
                new SnavelyReprojectionError(observed_x, observed_y)));
  }
  double observed_x;
  double observed_y;
};

struct SemiFixedCameraError {
  SemiFixedCameraError(const double * ref, int nobs, int ncam)
      : x(ref[0]), y(ref[1]), z(ref[2]), rotX(ref[3]), rotY(ref[4]), rotZ(ref[5]), nobs(nobs), ncam(ncam) {}
  template <typename T>
  bool operator()(const T* const camera,
                  T* residuals) const {
    double weight_x = 0.0  ; double weight_rotX = 0.15 ;
    double weight_y = 0.0  ; double weight_rotY = 0.15 ;
    double weight_z = 0.25 ; double weight_rotZ = 0.0  ;

    residuals[0] = weight_x    * ((double)nobs/(double)ncam) * (camera[0] - x);
    residuals[1] = weight_y    * ((double)nobs/(double)ncam) * (camera[1] - y);
    residuals[2] = weight_z    * ((double)nobs/(double)ncam) * (camera[2] - z);
    residuals[3] = weight_rotX * ((double)nobs/(double)ncam) * (camera[3] - rotX);
    residuals[4] = weight_rotY * ((double)nobs/(double)ncam) * (camera[4] - rotY);
    residuals[5] = weight_rotZ * ((double)nobs/(double)ncam) * (camera[5] - rotZ);
    return true;
  }
  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(const double * ref, const int nobs, const int ncam) {
    return (new ceres::AutoDiffCostFunction<SemiFixedCameraError, 6, 6>(
                new SemiFixedCameraError(ref, nobs, ncam)));
  }
  double x; double y; double z; double rotX; double rotY; double rotZ;
  int nobs; int ncam;
};

BundleAdjuster::BundleAdjuster()
{
  bundle_channel = nh.resolveName("bundle");
  bundle_sub     = nh.subscribe(bundle_channel, 10, &BundleAdjuster::bundleCb, this);

  // Publishers
  bundled_channel = nh.resolveName("bundled");
  bundled_pub     = nh.advertise<boris_drone::BundleMsg>(bundled_channel, 1);

  ros::param::get("~bundle_adjustment_tol", tolerance);
  ros::param::get("~quiet_ba", quiet_ba);
}

void BundleAdjuster::publishBundle(const BALProblem& bal_problem, bool converged,
            std::vector<double>& cost_of_point, double time_taken)
{
  boris_drone::BundleMsg msg = *(bal_problem.bundleMsgPtr_);
  int ncam = bal_problem.num_cameras_ ;  int npt  = bal_problem.num_points_;
  msg.num_cameras = ncam              ;  msg.num_points  = npt;
  msg.cameras.resize(ncam)            ;  msg.points.resize(npt);
  msg.converged = converged;
  msg.time_taken = time_taken;
  msg.cost_of_point.resize(npt);
  for (int i = 0; i < ncam; ++i) {
    msg.cameras[i].x    = bal_problem.parameters_[6*i + 0];
    msg.cameras[i].y    = bal_problem.parameters_[6*i + 1];
    msg.cameras[i].z    = bal_problem.parameters_[6*i + 2];
    msg.cameras[i].rotX = bal_problem.parameters_[6*i + 3]*PI/180.0;
    msg.cameras[i].rotY = bal_problem.parameters_[6*i + 4]*PI/180.0;
    msg.cameras[i].rotZ = bal_problem.parameters_[6*i + 5]*PI/180.0;
  }
  for (int i = 0; i < npt; ++i) {
    msg.points[i].x = bal_problem.parameters_[6*ncam + 3*i + 0];
    msg.points[i].y = bal_problem.parameters_[6*ncam + 3*i + 1];
    msg.points[i].z = bal_problem.parameters_[6*ncam + 3*i + 2];
    msg.cost_of_point[i] = cost_of_point[i];
  }
  bundled_pub.publish(msg);
}

BundleAdjuster::~BundleAdjuster()
{
}


void BundleAdjuster::bundleCb(const boris_drone::BundleMsg::ConstPtr bundlePtr)
{
  //Inspired by example code for bundle adjustment of Ceres (main function)
  //google::InitGoogleLogging(argv[0]);
  BALProblem bal_problem = BALProblem(bundlePtr);
  bool is_first_pass = bundlePtr->is_first_pass;

  double* camera_print;
  if (!quiet_ba)
  {
    ROS_INFO("Cameras before BA:");
    for (int i = 0; i< bal_problem.num_cameras_; i++)
    {
      camera_print = bal_problem.mutable_camera(i);
      ROS_INFO("       T       |       R       ");
      ROS_INFO("T : x  = % 5.2f | roll  = % 7.2f",camera_print[0],camera_print[3]);
      ROS_INFO("R : y  = % 5.2f | pitch = % 7.2f",camera_print[1],camera_print[4]);
      ROS_INFO("R : z  = % 5.2f | yaw   = % 7.2f",camera_print[2],camera_print[5]);
    }
  }

  double x, y;
  int i, nobs, ncam, nconstcams;
  double* camera;
  double* ref_pose;
  double* point;
  ceres::Problem problem;
  nobs = bal_problem.num_observations();
  ncam = bal_problem.num_cameras_;
  ceres::CostFunction* cost_function;
  for ( i = 0; i < nobs; ++i) {
    camera = bal_problem.mutable_camera_for_observation(i);
    point  = bal_problem.mutable_point_for_observation(i);
    x = bal_problem.observations()[2 * i + 0];
    y = bal_problem.observations()[2 * i + 1];
    cost_function = SnavelyReprojectionError::Create(x, y);
    ceres::LossFunction* loss_function = new ceres::HuberLoss(1.0);
    problem.AddResidualBlock(cost_function, loss_function, camera, point);
  }
  int n_constcams = ncam>4 ? ncam / 3 : 1;
  n_constcams = 1;

  for (int i = 0; i < ncam; ++i)
  {
    camera   = bal_problem.mutable_camera(i);
    ref_pose = bal_problem.ref_pose(i);
    if (i<n_constcams)
      problem.SetParameterBlockConstant(camera);
    else
    {
      cost_function = SemiFixedCameraError::Create(ref_pose,nobs,ncam);
      problem.AddResidualBlock(cost_function, NULL, camera);
    }
  }
  std::vector<double> cost_of_point;
  computeResiduals(problem, bal_problem, cost_of_point);

  ceres::Solver::Options options;
  options.max_num_iterations = is_first_pass? 30 : 50;
  options.linear_solver_type = ceres::DENSE_SCHUR;
  //TODO this is temporary
  options.minimizer_progress_to_stdout = !quiet_ba;
  options.function_tolerance = tolerance;
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  bool converged = (summary.termination_type == ceres::CONVERGENCE);
  double time_taken = summary.total_time_in_seconds;

  if (!quiet_ba)
  {
    std::cout << summary.FullReport() << "\n";
    ROS_INFO("Cameras after BA:");
    for (int i = 0; i< bal_problem.num_cameras_; i++)
    {
      camera_print = bal_problem.mutable_camera(i);
      ROS_INFO("       T       |       R       ");
      ROS_INFO("T : x  = % 5.2f | roll  = % 7.2f",camera_print[0],camera_print[3]);
      ROS_INFO("R : y  = % 5.2f | pitch = % 7.2f",camera_print[1],camera_print[4]);
      ROS_INFO("R : z  = % 5.2f | yaw   = % 7.2f",camera_print[2],camera_print[5]);
    }
  }
  computeResiduals(problem, bal_problem, cost_of_point);
  publishBundle(bal_problem, converged, cost_of_point, time_taken);
}

void BundleAdjuster::computeResiduals(ceres::Problem& problem, BALProblem& bal_problem,
      std::vector<double>& cost_of_point)
{
  double cost;
  std::vector<double> residuals;
  problem.Evaluate(ceres::Problem::EvaluateOptions(), &cost, &residuals, NULL, NULL);
  int nobs = bal_problem.num_observations_;
  int npt  = bal_problem.num_points_;
  double sqloss;
  std::vector<double> sqloss_of_point;
  std::vector<int> observations_of_point;
  sqloss_of_point.resize(npt,0.0);
  observations_of_point.resize(npt,0);
  cost_of_point.resize(npt);
  for (int i = 0; i<nobs; ++i)
  {
    sqloss = residuals[2*i]*residuals[2*i] + residuals[2*i+1]*residuals[2*i+1];
    sqloss_of_point[bal_problem.point_index_[i]] += sqloss;
    observations_of_point[bal_problem.point_index_[i]]++;
  }
  for (int i = 0; i < npt; ++i)
  {
    cost_of_point[i] = sqloss_of_point[i]/(double)observations_of_point[i];
    //ROS_INFO("Pt %d: cost = %f nobs = %d cost/obs = %f",
    //i,sqloss_of_point[i],observations_of_point[i],cost_of_point[i]);
  }
}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "bundle_adjuster");
  BundleAdjuster bundler_node;
  ros::Rate r(3);
  while (ros::ok())
  {
    ros::spinOnce();
    r.sleep();
  }
  return 0;
}
