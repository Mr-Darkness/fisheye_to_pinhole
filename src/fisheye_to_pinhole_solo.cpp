/*
 * Created on Sat Oct 12 2019
 *
 * Copyright (c) 2019 HITSZ-NRSL
 * All rights reserved
 *
 * Author: EpsAvlc
 */

#include <iostream>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <cv_bridge/cv_bridge.h>

using namespace std;
using namespace cv;

struct PolyfisheyeIntrins
{
    double A11;
    double A12;
    double A22;
    double u0;
    double v0;
    double k_2_7[6]; // k2-k7
};

/**
 * @brief Project pinhole's pixel pt into fisheye's pixel pt.
 * 
 * @param pt virtual pinhole camera's pixel point
 * @param pinhole_k pinhole's intrins
 * @param R pinhole's rotation w.r.t fisheye camera.
 * @param t pinhole's translation w.r.t fisheye camera.
 * @param intrins fisheye's intrins
 * @return Eigen::Vector2d fisheye camera's pixel pt
 */
Eigen::Vector2d ProjPinholeToFisheye(Eigen::Vector2d pt, Eigen::Matrix3d pinhole_k, Eigen::Matrix3d R, Eigen::Vector3d t, PolyfisheyeIntrins& intrins)
{

    // Back project pinhole points.
    double pinhole_fx = pinhole_k(0, 0);
    double pinhole_fy = pinhole_k(1, 1);
    double pinhole_u0 = pinhole_k(0, 2);
    double pinhole_v0 = pinhole_k(1, 2);

    Eigen::Vector3d obj_pt;
    obj_pt.x() = (pt.x() - pinhole_u0) / pinhole_fx;
    obj_pt.y() = (pt.y() - pinhole_v0) / pinhole_fy;
    obj_pt.z() = 1;
    obj_pt = R * obj_pt;

    // Project points into fisheye camera.
    double theta = acos(obj_pt(2) / obj_pt.norm());
    double inverse_r_P2 = 1.0 / sqrt(obj_pt(1) * obj_pt(1) + obj_pt(0) * obj_pt( 0 ) );
    double sin_phi = obj_pt(1) * inverse_r_P2;
    double cos_phi = obj_pt(0) * inverse_r_P2;

    double R0_theta = theta;
    double thetas = theta*theta;
    for(int i = 2; i <= 7; i++)
    {
        R0_theta += intrins.k_2_7[i-2] * thetas;
        thetas *= theta;
    }

    Eigen::Matrix2d Af = Eigen::Matrix2d::Zero();
    Af(0, 0) = intrins.A11;
    Af(0, 1) = intrins.A12;
    Af(1, 1) = intrins.A22;

    Eigen::Vector2d u = R0_theta*Af*Eigen::Vector2d(cos_phi, sin_phi) + Eigen::Vector2d(intrins.u0, intrins.v0);

    return u;
}

// Since it's not a good habit to use global variables, it's convenient for a simple node use.
Mat mapX, mapY;
ros::Publisher img_pub, cam_info_pub;
sensor_msgs::CameraInfo cam_info;

void Callback(const sensor_msgs::Image::ConstPtr& img_ptr)
{
    cv_bridge::CvImageConstPtr ros_img = cv_bridge::toCvShare(img_ptr);
    Mat img;
    remap(ros_img->image, img, mapX, mapY, INTER_LINEAR);

    sensor_msgs::ImagePtr img_msg = cv_bridge::CvImage(img_ptr->header, "mono8", img).toImageMsg();

    cam_info.header = img_ptr->header;

    img_pub.publish(img_msg);
    cam_info_pub.publish(cam_info);
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "fisheye_to_pinhole");

    ros::NodeHandle nh(""), nh_local;

    // Get parameters.
    PolyfisheyeIntrins intrins;
    int pinhole_image_width, pinhole_image_height;
    double pinhole_fx, pinhole_fy;
    string camera_topic;
    vector<double> fisheye_camera_T_data;

    bool is_param_get = true;
    is_param_get &= nh_local.getParam("projection_parameters/A11", intrins.A11);
    is_param_get &= nh_local.getParam("projection_parameters/A12", intrins.A12);
    is_param_get &= nh_local.getParam("projection_parameters/A22", intrins.A22);
    is_param_get &= nh_local.getParam("projection_parameters/k2", intrins.k_2_7[0]);
    is_param_get &= nh_local.getParam("projection_parameters/k3", intrins.k_2_7[1]);
    is_param_get &= nh_local.getParam("projection_parameters/k4", intrins.k_2_7[2]);
    is_param_get &= nh_local.getParam("projection_parameters/k5", intrins.k_2_7[3]);
    is_param_get &= nh_local.getParam("projection_parameters/k6", intrins.k_2_7[4]);
    is_param_get &= nh_local.getParam("projection_parameters/k7", intrins.k_2_7[5]);
    is_param_get &= nh_local.getParam("projection_parameters/u0", intrins.u0);
    is_param_get &= nh_local.getParam("projection_parameters/v0", intrins.v0);
    is_param_get &= nh_local.getParam("pinhole_image_width", pinhole_image_width);
    is_param_get &= nh_local.getParam("pinhole_image_height", pinhole_image_height);
    is_param_get &= nh_local.getParam("pinhole_fx", pinhole_fx);
    is_param_get &= nh_local.getParam("pinhole_fy", pinhole_fy);
    is_param_get &= nh_local.getParam("camera_topic", camera_topic);
    is_param_get &= nh_local.getParam("fisheye_camera_T", fisheye_camera_T_data);

    if(!is_param_get)
    {
        cout << "Some parameters read failed.";
        return -1;
    }

    Eigen::Matrix4d fisheye_camera_T = Eigen::Matrix4d::Zero();
    
    for(int i = 0; i < 3; i++)
        for(int j = 0; j < 4; j++)
        {
            fisheye_camera_T(i, j) = fisheye_camera_T_data[i*4+j];
        }
    fisheye_camera_T(3, 3) = 1;

    // Set virtual camera information
    Eigen::Matrix3d pinhole_k = Eigen::Matrix3d::Zero();
    pinhole_k(0, 0) = pinhole_fx;
    pinhole_k(1, 1) = pinhole_fy;
    pinhole_k(0, 2) = pinhole_image_width / 2;
    pinhole_k(1, 2) = pinhole_image_height / 2;
    pinhole_k(2, 2) = 1;

    mapX = Mat::zeros(pinhole_image_height, pinhole_image_width, CV_32F);
    mapY = Mat::zeros(pinhole_image_height, pinhole_image_width, CV_32F);

    Eigen::AngleAxisd angle(0, Eigen::Vector3d(0,-1,0));
    Eigen::Matrix3d R = angle.matrix();
    cout << R << endl;

    cam_info.height = pinhole_image_height;
    cam_info.width = pinhole_image_width;
    for(int i = 0; i < 9; i++)
        cam_info.K[i] = pinhole_k(i);
    cam_info.D = vector<double>(4, 0);    // No distortion
    Eigen::MatrixXd T(3, 4), proj(3, 4);

    T.block(0, 0, 3, 3) = R * fisheye_camera_T.block(0, 0, 3, 3);
    T.block(0, 3, 3, 1) = fisheye_camera_T.block(0, 3, 3, 1);
    proj = pinhole_k * T; // P = K * T
    for(int i = 0; i < 9; i++)
        cam_info.R[i] = T(i); 
    for(int i = 0; i < 12; i++)
        cam_info.P[i] = proj(i);

    // Start construct remapping relationship.
    for(int i = 0; i < pinhole_image_height; i++)
        for(int j = 0; j < pinhole_image_width; j++)
        {
            Eigen::Vector2d map_pt = ProjPinholeToFisheye(Eigen::Vector2d(j, i), pinhole_k, R, Eigen::Vector3d::Zero(), intrins);
            mapX.at<float>(i, j) = map_pt.x();
            mapY.at<float>(i, j) = map_pt.y();
        }

    ROS_INFO("Finish computing mapping relations. Start subscribe image topics.");

    ros::Subscriber sub = nh.subscribe(camera_topic, 1, &Callback);
    img_pub = nh.advertise<sensor_msgs::Image>("img", 1);
    cam_info_pub = nh.advertise<sensor_msgs::CameraInfo>("cam_info", 1);
    ros::spin();
    return 0;
}