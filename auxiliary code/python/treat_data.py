import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import rosbag
import subprocess, yaml
import rosbag_pandas
import sys
import os

def treat_data(file_in,do_plot):
    infile = file_in
    ref_pose_x    = 0
    ref_pose_y    = 0
    ref_pose_z    = 0
    ref_pose_rotZ = 0
    errors = np.zeros((4,5))
    counts = np.zeros((4,5))
    batimes = np.zeros((3,2))
    ind = -1
    starttime = -1
    k = 0
    for topic, msg, t in rosbag.Bag(infile).read_messages():
        if starttime == -1:
            starttime = t.secs
        if topic == "/pose_estimation":
            new_interval  = (ref_pose_x != msg.x and msg.x != 0)
            #if new_interval:
                #print(t.secs - starttime)
            ind += new_interval
            ref_pose_x    = msg.x
            ref_pose_y    = msg.y
            ref_pose_z    = msg.z
            ref_pose_rotZ = msg.rotZ
        if (topic == "/pose_visual" and ref_pose_x != 0):
            errX    = ref_pose_x    - msg.x
            errY    = ref_pose_y    - msg.y
            errD    = (errX**2 + errY**2)**(0.5)
            errrotZ = ref_pose_rotZ - msg.rotZ
            errors[0,ind] += abs(errX)
            errors[1,ind] += abs(errY)
            errors[2,ind] += abs(errD)
            errors[3,ind] += abs(errrotZ)
            counts[:,ind] += 1
        if topic == "/benchmark":
            batimes[k,0] = msg.BA_times_pass1[k]
            batimes[k,1] = msg.BA_times_pass2[k]
            k+=1
    #print(batimes)
    batime  = sum(sum(batimes))
    avgerrD = sum(errors[2,])/sum(counts[2,])
    avgerrR = sum(errors[3,])/sum(counts[3,])
    avgerr = sum(sum(errors))/sum(sum(counts))
    vis = rosbag_pandas.bag_to_dataframe(infile,include="/pose_visual")
    man = rosbag_pandas.bag_to_dataframe(infile,include="/pose_estimation")
    if do_plot:
        f, (axx, axy, axz, axr) = plt.subplots(4, sharex=True)
        #f, (axx, axy, axr) = plt.subplots(3, sharex=True)
        axx.plot(man.index, man['pose_estimation__x'])
        axx.plot(vis.index, vis['pose_visual__x'])
        axy.plot(man.index, man['pose_estimation__y'])
        axy.plot(vis.index, vis['pose_visual__y'])
        axz.plot(man.index, man['pose_estimation__z'])
        axz.plot(vis.index, vis['pose_visual__z'])
        axr.plot(man.index, man['pose_estimation__rotZ'])
        axr.plot(vis.index, vis['pose_visual__rotZ'])
        axx.set_title('X')
        axy.set_title('Y')
        axz.set_title('Z')
        axr.set_title('rotZ')
    return (avgerrD,avgerrR,batime)

def main():
    fn = "result_benchmark_noBA_false_robust_true.bag"
    avgerrD,avgerrR,batime = treat_data('/home/bor/bagfiles/otherpcresults/' + fn,True)
    print(fn)
    print(avgerrD)
    print(avgerrR)
    print(batime)

    fn = "result_benchmark_noBA_true_robust_true.bag"
    avgerrD,avgerrR,batime = treat_data('/home/bor/bagfiles/otherpcresults/' + fn,True)
    print(fn)
    print(avgerrD)
    print(avgerrR)
    print(batime)


    plt.show()
