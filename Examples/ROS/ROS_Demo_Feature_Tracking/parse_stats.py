import re
import numpy as np

# Example line:
# T: 1403715283.362143, RefKey Num: 500, patchMatchPredict Num: 495, Geo. valid: 492, Pred. suc. rate: 99.3939%, feature track rate: 98.4%, IMU num: 10, track_score: 5728.83, recall rate: 99%
# Pred. suc. rate    = Geo. valid / patchMatchPredict Num
# feature track rate = Geo. valid / RefKey Num

folder_path = 'ov_core/pixel_aware_gyro_aided_klt_feature_tracker/Examples/ROS/ROS_Demo_Feature_Tracking/bin/outfiles/'
# file_name = 'trackFeatures_gyro.txt'
file_name = 'trackFeatures_opencv.txt'

times = []
ref_key_num = []
patch_match_predict_num = []
geo_valid = []
lost_feats = []
frame_counter = 0

def get_number(pattern, string):
    match = re.search(pattern, string)
    return float(match.group(1))

with open(folder_path + file_name) as f:
    for line in f.readlines():
        times.append(get_number(r'T: (.*?),', line))
        ref_key_num.append(get_number(r'RefKey Num: (.*?),', line))
        patch_match_predict_num.append(get_number(r'patchMatchPredict Num: (.*?),', line))
        geo_valid.append(get_number(r'Geo. valid: (.*?),', line))

        lost_feats.append(ref_key_num[-1] - geo_valid[-1])
        frame_counter += 1

runtime = times[-1] - times[0]
print(f'runtime: {runtime:.2f}, num_frames: {frame_counter}, fps: {frame_counter/runtime:.2f}')
print(f'average lost_feats/frame: {np.mean(lost_feats):.2f}')