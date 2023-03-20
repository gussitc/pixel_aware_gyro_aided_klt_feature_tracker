#include "gyro_aided_tracker.h"
#include "patch_match.h"
#include <stack>
#include <thread>
#include <time.h>

const float GyroAidedTracker::TH_NCC_HIGH = 0.6f; //0.85f;
const float GyroAidedTracker::TH_NCC_LOW = 0.3f; // 0.65f;
const float GyroAidedTracker::TH_RATIO = 0.75f;
const char* GyroAidedTracker::TRACK_FEATURES_FILE_NAME = "trackFeatures.txt";
const char* GyroAidedTracker::TIME_COST_FILE_NAME = "timeCost.txt";

GyroAidedTracker::GyroAidedTracker(double t, double t_ref, const cv::Mat &imgGrayRef_, const cv::Mat &imgGrayCur_,
                                   const std::vector<cv::KeyPoint> &vKeysRef_, const std::vector<cv::KeyPoint> &vKeysCur_,
                                   const std::vector<cv::KeyPoint> &vKeysUnRef_, const std::vector<cv::KeyPoint> &vKeysUnCur_,
                                   const std::vector<IMU::Point> &vImuFromLastFrame,
                                   const IMU::Calib& imuCalib, const cv::Point3f &bias_,
                                   cv::Mat K_, cv::Mat DistCoef_, const cv::Mat &normalizeTable_,
                                   eType type_, ePredictMethod predictMethod_,
                                   std::string saveFolderPath, int halfPatchSize_
                                   ):
    mTimeStamp(t), mTimeStampRef(t_ref), mImgGrayRef(imgGrayRef_), mImgGrayCur(imgGrayCur_),
    mvKeysRef(vKeysRef_), mvKeysCur(vKeysCur_),
    mvKeysRefUn(vKeysUnRef_), mvKeysCurUn(vKeysUnCur_),
    mvImuFromLastFrame(vImuFromLastFrame),
    mRbc(imuCalib.Tbc.colRange(0,3).rowRange(0,3)),
    mBias(bias_),
    mK(K_), mDistCoef(DistCoef_), mWidth(imgGrayCur_.cols), mHeight(imgGrayCur_.rows),
    mNormalizeTable(normalizeTable_), mType(type_), mSaveFolderPath(saveFolderPath),
    mHalfPatchSize(halfPatchSize_), mPredictMethod(predictMethod_)
{
    assert(vImuFromLastFrame[0].t >= t_ref && vImuFromLastFrame[vImuFromLastFrame.size() - 1].t < t);
    Initialize();
}

GyroAidedTracker::GyroAidedTracker(const Frame& pFrameRef, const Frame& pFrameCur,
                                   const IMU::Calib& imuCalib,
                                   const cv::Point3f &biasg_,
                                   const cv::Mat &normalizeTable_,
                                   eType type_,
                                   ePredictMethod predictMethod_,
                                   std::string saveFolderPath,
                                   int halfPatchSize_):
    mTimeStamp(pFrameCur.mTimeStamp), mTimeStampRef(pFrameRef.mTimeStamp), mImgGrayRef(pFrameRef.mGray), mImgGrayCur(pFrameCur.mGray),
    mvKeysRef(pFrameRef.mvKeys), mvKeysCur(pFrameCur.mvKeys),
    mvKeysRefUn(pFrameRef.mvKeysUn), mvKeysCurUn(pFrameCur.mvKeysUn),
    mvImuFromLastFrame(pFrameCur.mvImuFromLastFrame),
    mRbc(imuCalib.Tbc.colRange(0,3).rowRange(0,3)),
    mBias(biasg_),
    mK(pFrameCur.mpCameraParams->mK), mDistCoef(pFrameCur.mpCameraParams->mDistCoef), mWidth(pFrameCur.mpCameraParams->width), mHeight(pFrameCur.mpCameraParams->height),
    mNormalizeTable(normalizeTable_), mType(type_), mSaveFolderPath(saveFolderPath),
    mHalfPatchSize(halfPatchSize_),  mPredictMethod(predictMethod_)
{
    Initialize();
}

void GyroAidedTracker::Initialize()
{
    // create folder
    if (mSaveFolderPath.size() > 0){    // if the folder path is set, then we save processing results
        //mSaveFolderPath = mSaveFolderPath + "/mType_" + std::to_string(mType) + "_mPredictMethod_" + std::to_string(mPredictMethod) + "/";
        std::string command = "mkdir -p " + mSaveFolderPath;
        int a = system(command.c_str());
    }

    mbNCC = true;
    mHalfPatchSize = mHalfPatchSize == 0? 5: mHalfPatchSize;
    mRadiusForFindNearNeighbor = 2 * mHalfPatchSize; //4.0f;

    mfx = mK.at<float>(0,0); mfy = mK.at<float>(1,1);
    mcx = mK.at<float>(0,2); mcy = mK.at<float>(1,2);
    mfx_inv = 1.0 / mfx; mfy_inv = 1.0 / mfy;

    mk1 = mDistCoef.at<float>(0); mk2 = mDistCoef.at<float>(1);
    mp1 = mDistCoef.at<float>(2); mp2 = mDistCoef.at<float>(3);
    mk3 = mDistCoef.total() == 5? mDistCoef.at<float>(4): 0;

    mvPatchCorners.resize(4);
    mvPatchCorners[0] = cv::Point2f(- mHalfPatchSize, - mHalfPatchSize);// top left
    mvPatchCorners[1] = cv::Point2f(mHalfPatchSize, - mHalfPatchSize);  // top right
    mvPatchCorners[2] = cv::Point2f(- mHalfPatchSize, mHalfPatchSize);  // bottom left
    mvPatchCorners[3] = cv::Point2f(mHalfPatchSize, mHalfPatchSize);    // botton right
    mMatPatchCorners = cv::Mat(mvPatchCorners).reshape(1).t();    // matB is used for predicting the affine deformation matrix

    mN = mvKeysRef.size();

    mvPtPredict = std::vector<cv::Point2f>(mN, cv::Point2f(0,0));
    mvPtPredictUn = std::vector<cv::Point2f>(mN, cv::Point2f(0,0));
    mvFlowsPredictUn = std::vector<cv::Point2f>(mN, cv::Point2f(0,0));
    mvStatus = std::vector<uchar>(mN, false);
    mvError.resize(mvKeysRef.size());
    mvDisparities.reserve(mN);
    mvMatches.reserve(mN);
    mvvNearNeighbors.resize(mN);

    // corners of the predict pixels
    mvvPtPredictCorners.resize(mN);
    mvvPtPredictCornersUn.resize(mN);
    mvvFlowsPredictCorners.resize(mN);
    mvAffineDeformationMatrix.resize(mN);
}

void GyroAidedTracker::SetBackToFrame(Frame &pFrame)
{
    pFrame.mvPtGyroPredictUn = std::vector<cv::Point2f>(mvPtGyroPredictUn.begin(), mvPtGyroPredictUn.end());
    pFrame.mvPtPredict = std::vector<cv::Point2f>(mvPtPredict.begin(), mvPtPredict.end());
    pFrame.mvPtPredictUn = std::vector<cv::Point2f>(mvPtPredictUn.begin(), mvPtPredictUn.end());
    pFrame.mvStatus = std::vector<uchar>(mvStatus.begin(), mvStatus.end());
    pFrame.mvNcc = std::vector<float>(mvNccAfterPatchMatched.begin(), mvNccAfterPatchMatched.end());

    pFrame.mvvFlowsPredictCorners.resize(mvvFlowsPredictCorners.size());
    for(size_t i = 0, iend = mvvFlowsPredictCorners.size(); i < iend; i++){
        pFrame.mvvFlowsPredictCorners[i] = std::vector<cv::Point2f>(mvvFlowsPredictCorners[i].begin(), mvvFlowsPredictCorners[i].end());
    }

    pFrame.mRcl = mRcl.clone();
}

/**
 * @brief GyroAidedTracker::GyroPredictFeatures
 * @return The number of predicted features.
 * Predict features using gyroscope integrated rotation (Rcl), do not use depth and translation
 */
int GyroAidedTracker::GyroPredictFeatures()
{
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    cv::parallel_for_(cv::Range(0,mN), [&](const cv::Range& range){
        for (auto i = range.start; i < range.end; i++){
            cv::Point2f pt_ref_un = mvKeysRefUn[i].pt;
            cv::Point2f pt_predict_un;
            cv::Point2f pt_predict_distort;
            cv::Point2f flow;
            GyroPredictOnePixel(pt_ref_un, pt_predict_un, pt_predict_distort, flow);

            // Check boarder.
            if (pt_predict_un.x < 0 || pt_predict_un.x >= mWidth || pt_predict_un.y < 0 || pt_predict_un.y >= mHeight)
                continue;

            if (pt_predict_distort.x < 0 || pt_predict_distort.x >= mWidth || pt_predict_distort.y < 0 || pt_predict_distort.y >= mHeight)
                continue;

            mvPtPredictUn[i] = pt_predict_un;
            mvPtPredict[i] = pt_predict_distort; // Distorted
            mvStatus[i] = true;
            mvFlowsPredictUn[i] = flow;

            // Predict four corners on undistorted image
            std::vector<cv::Point2f> vPtPredictCorners;
            std::vector<cv::Point2f> vPtPredictCornesUn;
            std::vector<cv::Point2f> vecC;  // predicted corner - center point

            // Predict the four corners of the patch window
            for (int j = 0; j < mvPatchCorners.size(); j++) {
                cv::Point2f pt_corner_un(mvKeysRefUn[i].pt + mvPatchCorners[j]);

                cv::Point2f pt_predict_corner_un;
                cv::Point2f pt_predict_corner_distort;
                cv::Point2f flow_corner;
                GyroPredictOnePixel(pt_corner_un, pt_predict_corner_un, pt_predict_corner_distort, flow_corner);
                vPtPredictCornesUn.push_back(pt_predict_corner_un);
                vPtPredictCorners.push_back(pt_predict_corner_distort);

                // Since the affine matrix is applied on raw image, we use the distorted corners to estimate the matrix.
                vecC.push_back(pt_predict_corner_un - pt_predict_un);
            }
            mvvPtPredictCorners[i] = vPtPredictCorners;
            mvvPtPredictCornersUn[i] = vPtPredictCornesUn;
            mvvFlowsPredictCorners[i] = vecC;

            // Predict the affine deformation matrix A. A = [1 + dxx, dxy; dyx, 1 + dyy]. Note: on undistort image
            cv::Mat matC = cv::Mat(vecC).reshape(1).t();
            cv::Mat A = matC * mMatPatchCorners.t() * (mMatPatchCorners * mMatPatchCorners.t()).inv();
            mvAffineDeformationMatrix[i] = A;   // The affine matrix is applied on raw image
        }
    });

    int n_predict = 0;
    for (auto state: mvStatus){
        if (state)
            n_predict ++;
    }

    std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
    mTimeCostGyroPredict = std::chrono::duration_cast<std::chrono::duration<float> >(t2 - t1).count();

    mvPtGyroPredict = std::vector<cv::Point2f>(mvPtPredict.begin(), mvPtPredict.end());
    mvPtGyroPredictUn = std::vector<cv::Point2f>(mvPtPredictUn.begin(), mvPtPredictUn.end());

    return n_predict;
}

/**
 * @brief GyroAidedTracker::GyroPredictOnePixel
 * @param pt_ref                Undistorted feature point on reference frame.
 * @param pt_predict            Predicted point on current frame.
 * @param pt_predict_distort    Distorted the predicted point. Used to image show.
 * @param flow                  Optical Flow. flow = pt_redict - pt_ref
 */
void GyroAidedTracker::GyroPredictOnePixel(cv::Point2f &pt_ref,
                                           cv::Point2f &pt_predict,
                                           cv::Point2f &pt_predict_distort,
                                           cv::Point2f &flow)
{
    float x_normal, y_normal;
    // 2D pixel [u, v] --> normal [x_normal, y_normal]
    if (!mNormalizeTable.empty()) {
        // Use previously calculated normalize table to obtain normalized value --> performs well
        x_normal = mNormalizeTable.at<cv::Vec2f>(int(pt_ref.y), int(pt_ref.x))[0];
        y_normal = mNormalizeTable.at<cv::Vec2f>(int(pt_ref.y), int(pt_ref.x))[1];
    }
    else {
        // Calculate normalized value at run time --> performs well too
        // pixel to camera
        x_normal = (pt_ref.x - mcx) * mfx_inv;
        y_normal = (pt_ref.y - mcy) * mfy_inv;
    }

    if(mPredictMethod == PIXEL_AWARE_PREDICTION){   // the proposed, considering pixel coordinates
        // u2 = 1/(r3 * Kinv * u1 + t3) * K * R21 * Kinv * u1 + K * t21 / (r3 * Z1 * Kinv * u1 + t3)
        // ==> u2 approx 1/(r3 * Kinv * u1) * K * R21 * Kinv * u1
        float lambda = 1.0 / (mr31 * x_normal + mr32 * y_normal + mr33);
        float pt_x = (mKRKinv.at<float>(0,0) * pt_ref.x + mKRKinv.at<float>(0,1) * pt_ref.y + mKRKinv.at<float>(0,2)) * lambda;
        float pt_y = (mKRKinv.at<float>(1,0) * pt_ref.x + mKRKinv.at<float>(1,1) * pt_ref.y + mKRKinv.at<float>(1,2)) * lambda;
        pt_predict = cv::Point2f(pt_x, pt_y);

        float x = (pt_predict.x - mcx) * mfx_inv;
        float y = (pt_predict.y - mcy) * mfy_inv;

        float r2 = x * x + y * y;
        float r4 = r2 * r2;
        float r6 = r4 * r2;
        float x_distort = x * (1 + mk1 * r2 + mk2 * r4 + mk3 * r6) + 2 * mp1 * x * y + mp2 * (r2 + 2 * x * x);
        float y_distort = y * (1 + mk1 * r2 + mk2 * r4 + mk3 * r6) + mp1 * (r2 + 2 * y * y) + 2 * mp2 * x * y;
        float u_distort = mfx * x_distort + mcx;
        float v_distort = mfy * y_distort + mcy;
        pt_predict_distort = cv::Point2f(u_distort, v_distort); // Distorted

        flow = pt_predict - pt_ref; // optical flow on undistorted image
    }
    else if(mPredictMethod == SINGLE_HOMOGRAPHY){   // homography
        // u2 = K * R21 * Kinv * u1
        float lambda = 1.0;
        float pt_x = (mKRKinv.at<float>(0,0) * pt_ref.x + mKRKinv.at<float>(0,1) * pt_ref.y + mKRKinv.at<float>(0,2)) * lambda;
        float pt_y = (mKRKinv.at<float>(1,0) * pt_ref.x + mKRKinv.at<float>(1,1) * pt_ref.y + mKRKinv.at<float>(1,2)) * lambda;
        pt_predict = cv::Point2f(pt_x, pt_y);

        float x = (pt_predict.x - mcx) * mfx_inv;
        float y = (pt_predict.y - mcy) * mfy_inv;;

        float r2 = x * x + y * y;
        float r4 = r2 * r2;
        float r6 = r4 * r2;
        float x_distort = x * (1 + mk1 * r2 + mk2 * r4 + mk3 * r6) + 2 * mp1 * x * y + mp2 * (r2 + 2 * x * x);
        float y_distort = y * (1 + mk1 * r2 + mk2 * r4 + mk3 * r6) + mp1 * (r2 + 2 * y * y) + 2 * mp2 * x * y;
        float u_distort = mfx * x_distort + mcx;
        float v_distort = mfy * y_distort + mcy;
        pt_predict_distort = cv::Point2f(u_distort, v_distort); // Distorted

        flow = pt_predict - pt_ref; // optical flow on undistorted image
    }
}

int GyroAidedTracker::GyroPredictFeaturesAndOpticalFlowRefined()
{
    /// Step 1: Predict features using gyroscope integrated rotation
    if (mbHasGyroPredictInitial)
        int n_predict_gyro = GyroPredictFeatures();     // default: true
    else {
        for (int i = 0; i < mN; i++){
            mvPtPredictUn[i] = mvKeysRefUn[i].pt;
            mvPtPredict[i] = mvKeysRef[i].pt;
            mvStatus[i] = true;
            mvFlowsPredictUn[i] = cv::Point2f(0,0);
            mvAffineDeformationMatrix[i] = cv::Mat::eye(2,2,CV_32F);
        }
    }

    /// Step 2: For each gyro-perdicted points, use optical flow to refine its location.
    // for Opencv 3.4
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    int iterations = 10;
    int pyramids = 3;
    bool inverse = false;    // if false, the time cost is about 0.020s for tracking 800 features (performance: better)
    // if true, the time cost is about 0.010s (performance: worser)
    PatchMatch patchMatch(this, mHalfPatchSize, iterations, pyramids,
                          mbHasGyroPredictInitial, inverse, mbConsiderIllumination, mbConsiderAffineDeformation,
                          mbRegularizationPenalty);
    patchMatch.OpticalFlowMultiLevel();

    std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
    mTimeCostOptFlow = std::chrono::duration_cast<std::chrono::duration<float> >(t2 - t1).count();


    /// Step 3: Set patch matched results back to mvPredict and mvPredictUn.
    /// Step 3.1: Set thresholds for filtering out patch-matched refined pixels.
    std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now();
    int N = mvKeysRefUn.size();

    double sumPixelError = 0;
    double maxPixelError = 0;
    int cnt = 0;
    for (int i = 0; i < N; ++i) {
        if (mvStatusAfterPatchMatched[i]) {
            double pixelError = mvPixelErrorsOfPatchMatched[i];
            maxPixelError = maxPixelError < pixelError? pixelError : maxPixelError;
            sumPixelError += pixelError;
            cnt ++;
        }
    }
    double avgPixelError = sumPixelError / cnt;
    // thPixelError: threshold for filtering out patch-matched predicted pixels
    //               that have large pixel errors between the reference keypoints and the patch-matched results.
    double thPixelError = 4.0 * avgPixelError > mHalfPatchSize? 4.0 * avgPixelError: mHalfPatchSize;  // tolerate 3 times average pixel error.

    // thDistance: threshold for filtering out patch-matched predicted pixels
    //             that have large distance between gyro. predict results and patch-matched results.
    double thDistance = mHalfPatchSize * 4.0;

    /// Step 3.2: Filter out and set back to mvPredict, mvPtPredictUn, and mvStatus.
    maxPixelError = 0;
    double maxDist = 0, sumDist = 0;
    int n_predict = 0;
    for (size_t i = 0; i < N; i++) {
        double pixelError = mvPixelErrorsOfPatchMatched[i];
        double distance = mvDistanceBetweenPredictedAndPatchMatched[i];

        if (mvStatusAfterPatchMatched[i] && pixelError < thPixelError && distance < thDistance) {
            sumDist += distance;

            maxPixelError = maxPixelError < pixelError? pixelError : maxPixelError;
            maxDist = maxDist < distance? distance : maxDist;

            // Set back to mvPredict, mvPtPredictUn, and mvStatus
            mvPtPredict[i] = mvPtPredictAfterPatchMatched[i];
            mvPtPredictUn[i] = mvPtPredictAfterPatchMatchedUn[i];
            mvStatus[i] = true;
            n_predict ++;
        }else {
            mvStatus[i] = false;
        }
    }

    std::chrono::steady_clock::time_point t4 = std::chrono::steady_clock::now();
    mTimeCostOptFlowResultFilterOut = std::chrono::duration_cast<std::chrono::duration<float> >(t4 - t3).count();

    return n_predict;
}

int GyroAidedTracker::TrackFeatures()
{
    Timer timer, timer_total;
    IntegrateGyroMeasurements();
    double t_integrate = timer.runTime_s(); timer.freshTimer();

    int n_predict;

    /// Step 1: Predict features using gyroscope integrated rotation
    if(mType == OPENCV_OPTICAL_FLOW_PYR_LK){
        // using opencv optical flow pyr LK
        vector<cv::Point2f> pt_ref_detected;
        for_each(mvKeysRefUn.begin(), mvKeysRefUn.end(), [&](cv::KeyPoint kp){
            pt_ref_detected.push_back(kp.pt);
        });

        // opencv optical flow tracking
        cv::Size winSize = cv::Size(2 * mHalfPatchSize + 1, 2 * mHalfPatchSize + 1);
        int maxLevel = 2;
        cv::TermCriteria criteria = cv::TermCriteria(TermCriteria::COUNT+TermCriteria::EPS, 30, 0.01);
        // TODO: test OPTFLOW_USE_INITIAL_FLOW
        int flags = 0; //cv::OPTFLOW_USE_INITIAL_FLOW;
        double minEigThreshold = 1e-4;
        cv::calcOpticalFlowPyrLK(mImgGrayRef, mImgGrayCur, pt_ref_detected, mvPtPredictUn,
                                 mvStatus, mvError, winSize, maxLevel, criteria, flags, minEigThreshold);

        mvPtPredict.resize(mvPtPredictUn.size());
        for (size_t i = 0; i < mvStatus.size(); i++) {
            if(mvError[i] >= 12.0)
                mvStatus[i] = false;

            mvFlowsPredictUn[i] = mvPtPredictUn[i] - mvKeysRefUn[i].pt;
        }

        // distort points for display
        DistortVecPoints(mvPtPredictUn, mvPtPredict, mK, mDistCoef);
    }
    else if (mType == GYRO_PREDICT)
        n_predict = GyroPredictFeatures();
    else {
        if (mType == IMAGE_ONLY_OPTICAL_FLOW_CONSIDER_ILLUMINATION) {
            mbHasGyroPredictInitial = false;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = false;
        }
        else if (mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = false;
            mbConsiderAffineDeformation = false;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = false;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION_DEFORMATION) {
            // Default
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION_DEFORMATION_REGULAR) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = true;
        }
        else {
            LOG(ERROR) << "Unsupport type!!! return -1;";
            return -1;
        }

        n_predict = GyroPredictFeaturesAndOpticalFlowRefined();

        double t_predict = timer.runTime_s(); timer.freshTimer();
    }

    return n_predict;
}

// Step 2: use multi-view geometric to filter out outliers
int GyroAidedTracker::GeometryValidation()
{
    Timer timer;
    std::vector<cv::Point2f> vPts1, vPts2;
std:vector<int> vIndeces;
    for(size_t i = 0, iend = mvKeysRefUn.size(); i < iend; i++){
        if(mvStatus[i]){
            vIndeces.push_back(i);
            vPts1.push_back(mvKeysRefUn[i].pt);
            vPts2.push_back(mvPtPredictUn[i]);
        }
    }

    // Perform geometrical validation to filter out outliers
    int cnt_inlier = 0, cnt_outlier = 0;
    float track_score = 0;
    if(vPts1.size() > 8)
    {
        float sigma = 1.0;
        float score_F, score_H;
        std::vector<bool> vbMatchesInliers_F;
        std::vector<bool> vbMatchesInliers_H;
        cv::Mat F21, H21;

        std::thread threadH(&GyroAidedTracker::CheckHomography, this, std::ref(H21), std::ref(score_H), ref(vbMatchesInliers_H), ref(vPts1), ref(vPts2), ref(sigma));
        std::thread threadF(&GyroAidedTracker::CheckFundamental, this, std::ref(F21),std::ref(score_F), ref(vbMatchesInliers_F), ref(vPts1), ref(vPts2), ref(sigma));

        // Wait until both threads have finished
        threadH.join();
        threadF.join();

        float RH = score_H / (score_F + score_H);
        std::vector<bool> vbInliers;
        // Choose homography model or fundamental to remove outliers depending on ratio (0.40~0.45)
        if(RH > 0.45){
            vbInliers = vbMatchesInliers_H; // choose H
            track_score = score_H;
        }
        else{
            vbInliers = vbMatchesInliers_F; // choose F
            track_score = score_F;
        }


        for (size_t i = 0, iend = vIndeces.size(); i < iend; i++) {
            // Also check if the predicted points are actually inside the image
            if(!vbInliers[i] || (vPts2.at(i).x < 0 || vPts2.at(i).y < 0 || (int)vPts2.at(i).x >= mWidth || (int)vPts2.at(i).y >= mHeight)){
                mvStatus[vIndeces[i]] = false;  // mark outliers
                cnt_outlier ++;
            }
            else {
                cnt_inlier ++;
            }
        }
    }

    mTimeCostGeometryValidation = timer.runTime_s(); timer.freshTimer();
    mTImeCostTotalFeatureTrack = mTimeCostGyroPredict + mTimeCostOptFlow + mTimeCostOptFlowResultFilterOut + mTimeCostGeometryValidation;

    std::stringstream s1;
    s1 << "RefKey Num: " << mN << ", patchMatchPredict Num: " << vPts1.size()
       << ", Geo. valid: " << cnt_inlier
       << ", Pred. suc. rate: " << 100.0 * cnt_inlier / vPts1.size() << "%"
       << ", feature track rate: " << 100.0 * cnt_inlier / mN << "%"
       << ", IMU num: " << mvImuFromLastFrame.size()
       << ", track_score: " << track_score
       << ", recall rate: " << 100.0 * vPts1.size() / mN << "%"; // << std::endl;
    std::string msg1 = "T: " + std::to_string(mTimeStamp) + ", " + s1.str();
    SaveMsgToFile(TRACK_FEATURES_FILE_NAME, msg1);

    std::stringstream s2;
    s2  << "Total FeatureTrack: " << mTImeCostTotalFeatureTrack
        << ", GyroPredict: " << mTimeCostGyroPredict
        << ", OptFlow: " << mTimeCostOptFlow
        << ", OptFlowResultFilterOut: " << mTimeCostOptFlowResultFilterOut
        << ", GeometryValidation: " << mTimeCostGeometryValidation;// << std::endl;
    std::string msg2 = "T: " + std::to_string(mTimeStamp) + ", " + s2.str();
    SaveMsgToFile(TIME_COST_FILE_NAME, msg2);

    return cnt_inlier;
}


void GyroAidedTracker::SetRcl(const cv::Mat Rcl_)
{
    Rcl_.copyTo(mRcl);
    mr11 = mRcl.at<float>(0,0); mr12 = mRcl.at<float>(0,1); mr13 = mRcl.at<float>(0,2);
    mr21 = mRcl.at<float>(1,0); mr22 = mRcl.at<float>(1,1); mr23 = mRcl.at<float>(1,2);
    mr31 = mRcl.at<float>(2,0); mr32 = mRcl.at<float>(2,1); mr33 = mRcl.at<float>(2,2);

    mKRKinv = mK * mRcl * mK.inv();
}

void GyroAidedTracker::IntegrateGyroMeasurements()
{
    cv::Mat dR_ref_cur = cv::Mat::eye(3, 3, CV_32F);
    const int n = mvImuFromLastFrame.size()-1;

    // Consider the gap between the IMU timestamp and camera timestamp.
    for (int i = 0; i < n; i++) {
        float tstep;
        cv::Point3f angVel;
        if((i == 0) && (i < (n-1)))
        {
            float tab = mvImuFromLastFrame[i+1].t - mvImuFromLastFrame[i].t;
            float tini = mvImuFromLastFrame[i].t - mTimeStampRef;
            angVel = (mvImuFromLastFrame[i].w + mvImuFromLastFrame[i+1].w -
                    (mvImuFromLastFrame[i+1].w - mvImuFromLastFrame[i].w) * (tini/tab)) * 0.5f;
            tstep = mvImuFromLastFrame[i+1].t - mTimeStampRef;
        }
        else if(i < (n-1))
        {
            angVel = (mvImuFromLastFrame[i].w + mvImuFromLastFrame[i+1].w) * 0.5f;
            tstep = mvImuFromLastFrame[i+1].t - mvImuFromLastFrame[i].t;
        }
        else if((i > 0) && (i == (n-1)))
        {
            float tab = mvImuFromLastFrame[i+1].t - mvImuFromLastFrame[i].t;
            float tend = mvImuFromLastFrame[i+1].t - mTimeStamp;
            angVel = (mvImuFromLastFrame[i].w + mvImuFromLastFrame[i+1].w -
                    (mvImuFromLastFrame[i+1].w - mvImuFromLastFrame[i].w) * (tend / tab)) * 0.5f;
            tstep = mTimeStamp - mvImuFromLastFrame[i].t;
        }
        else if((i == 0) && (i == (n-1)))
        {
            angVel = mvImuFromLastFrame[i].w;
            tstep = mTimeStamp - mTimeStampRef;
        }

        dR_ref_cur *= IntegrateOneGyroMeasurement(angVel, tstep);
    }

    cv::Mat Rcl = mRbc.t() * dR_ref_cur.t() * mRbc;
    SetRcl(Rcl);
}

cv::Mat GyroAidedTracker::IntegrateOneGyroMeasurement(cv::Point3f &gyro, double dt)
{
    const float x = (gyro.x - mBias.x) * dt;
    const float y = (gyro.y - mBias.y) * dt;
    const float z = (gyro.z - mBias.z) * dt;

    const float d2 = x * x + y * y + z * z;
    const float d = std::sqrt(d2);

    cv::Mat I = cv::Mat::eye(3, 3, CV_32F);
    cv::Mat W = (cv::Mat_<float>(3,3) << 0, -z, y,
                 z, 0, -x,
                 -y, x, 0);

    cv::Mat deltaR;
    if (d < 1e-4){
        deltaR = I + W;         // on-manifold equation (4)
    }
    else {
        deltaR = I + W * std::sin(d) / d + W * W * (1.0f - std::cos(d)) / d2;           // on-manifold equation (3)
    }

    return deltaR;
}

float GyroAidedTracker::CheckHomography(
        cv::Mat &H21,
        float &score,
        std::vector<bool> &vbMatchesInliers,
        std::vector<cv::Point2f> &vPts1, std::vector<cv::Point2f> &vPts2,
        float sigma)
{
    cv::Mat mask;
    H21 =  cv::findHomography(vPts1, vPts2, cv::RANSAC, 3, mask);
    cv::Mat H12 = H21.inv();

    const double h11 = H21.at<double>(0,0);
    const double h12 = H21.at<double>(0,1);
    const double h13 = H21.at<double>(0,2);
    const double h21 = H21.at<double>(1,0);
    const double h22 = H21.at<double>(1,1);
    const double h23 = H21.at<double>(1,2);
    const double h31 = H21.at<double>(2,0);
    const double h32 = H21.at<double>(2,1);
    const double h33 = H21.at<double>(2,2);

    const double h11inv = H12.at<double>(0,0);
    const double h12inv = H12.at<double>(0,1);
    const double h13inv = H12.at<double>(0,2);
    const double h21inv = H12.at<double>(1,0);
    const double h22inv = H12.at<double>(1,1);
    const double h23inv = H12.at<double>(1,2);
    const double h31inv = H12.at<double>(2,0);
    const double h32inv = H12.at<double>(2,1);
    const double h33inv = H12.at<double>(2,2);

    const int N = vPts1.size();
    vbMatchesInliers.resize(N);

    score = 0;
    const float th = 5.99;
    const float invSigmaSquare = 1.0/(sigma*sigma);

    int cnt_outlier = 0;
    int cnt_sum = 0;
    for(size_t i = 0; i < N; i++){
        bool bIn = true;
        const cv::Point2f &pt1 = vPts1[i];
        const cv::Point2f &pt2 = vPts2[i];

        const float u1 = pt1.x;
        const float v1 = pt1.y;
        const float u2 = pt2.x;
        const float v2 = pt2.y;

        // Reprojection error in second image
        // x2 = H21 * x1
        const float w1in2inv = 1.0 / (h31 * u1 + h32 * v1 + h33);
        const float u1in2 = (h11 * u1 + h12 * v1 + h13) * w1in2inv;
        const float v1in2 = (h21 * u1 + h22 * v1 + h23) * w1in2inv;

        const float squareDist2 = (u2 - u1in2) * (u2 - u1in2) + (v2 - v1in2) * (v2 - v1in2);
        const float chiSquare2 = squareDist2 * invSigmaSquare;

        if(chiSquare2 > th)
            bIn = false;
        else{
            score += th - chiSquare2;
            cnt_sum ++;
        }

        // Reprojection error in the first image
        // x1 = H12 * x2
        const float w2in1inv = 1.0 / (h31inv * u2 + h32inv * v2 + h33inv);
        const float u2in1 = (h11inv * u2 + h12inv * v2 + h13inv) * w2in1inv;
        const float v2in1 = (h21inv * u2 + h22inv * v2 + h23inv) * w2in1inv;

        const float squareDist1 = (u1 - u2in1) * (u1 - u2in1) + (v1 - v2in1) * (v1 - v2in1);
        const float chiSquare1 = squareDist1 * invSigmaSquare;

        if(chiSquare1 > th)
            bIn = false;
        else{
            score += th - chiSquare1;
            cnt_sum ++;
        }

        if (bIn)
            vbMatchesInliers[i] = true;
        else{
            vbMatchesInliers[i] = false;
            cnt_outlier ++;
        }
    }

    return score;
}

float GyroAidedTracker::CheckFundamental(
        cv::Mat &F21,
        float &score,
        std::vector<bool> &vbMatchesInliers,
        std::vector<cv::Point2f> &vPts1,
        std::vector<cv::Point2f> &vPts2,
        float sigma)
{
    cv::Mat mask;
    F21 = cv::findFundamentalMat(vPts1, vPts2, cv::FM_RANSAC, 3., 0.99, mask);

    const double f11 = F21.at<double>(0,0);
    const double f12 = F21.at<double>(0,1);
    const double f13 = F21.at<double>(0,2);
    const double f21 = F21.at<double>(1,0);
    const double f22 = F21.at<double>(1,1);
    const double f23 = F21.at<double>(1,2);
    const double f31 = F21.at<double>(2,0);
    const double f32 = F21.at<double>(2,1);
    const double f33 = F21.at<double>(2,2);

    const int N = vPts1.size();
    vbMatchesInliers.resize(N);

    score = 0;
    const float th = 3.84;
    const float thScore = 5.99;
    const float invSigmaSquare = 1.0/(sigma*sigma);

    int cnt_outlier = 0;
    int cnt_sum = 0;
    for(size_t i = 0; i < N; i++){
        bool bIn = true;
        const cv::Point2f &pt1 = vPts1[i];
        const cv::Point2f &pt2 = vPts2[i];

        const float u1 = pt1.x;
        const float v1 = pt1.y;
        const float u2 = pt2.x;
        const float v2 = pt2.y;

        // Reprojection error in second image
        // l2 = F21 * p1
        const float a2 = f11 * u1 + f12 * v1 + f13;
        const float b2 = f21 * u1 + f22 * v1 + f23;
        const float c2 = f31 * u1 + f32 * v1 + f33;

        // square distance of p2 to l2
        const float num2 = a2 * u2 + b2 * v2 + c2;
        const float squareDist2 = num2 * num2 / (a2 * a2 + b2 * b2);
        const float chiSquare2 = squareDist2 * invSigmaSquare;

        if(chiSquare2 > th)
            bIn = false;
        else{
            score += thScore - chiSquare2;
            cnt_sum ++;
        }

        // Reprojection error in first image
        // l1 = p2^T * F21
        const float a1 = u2 * f11 + v2 * f21 + f31;
        const float b1 = u2 * f12 + v2 * f22 + f32;
        const float c1 = u2 * f13 + v2 * f23 + f33;

        // square distance of p1 to l1
        const float num1 = a1 * u1 + b1 * v1 + c1;
        const float squareDist1 = num1 * num1 / (a1 * a1 + b1 * b1);
        const float chiSquare1 = squareDist1 * invSigmaSquare;

        if (chiSquare1 > th)
            bIn = false;
        else{
            score += thScore - chiSquare1;
            cnt_sum ++;
        }

        if (bIn)
            vbMatchesInliers[i] = true;
        else{
            vbMatchesInliers[i] = false;
            cnt_outlier ++;
        }
    }

    return score;
}

void GyroAidedTracker::SaveMsgToFile(std::string filename, std::string &msg)
{
    std::ofstream fp(mSaveFolderPath + filename, ofstream::app);
    if(!fp.is_open()) LOG(ERROR) << RED"cannot open: " << filename << RESET;
    else {
        fp << std::fixed << std::setprecision(6);
        fp << msg << std::endl;
    }
}


/**
 * Find the neighbors for each predicted point. The neighbors are the detected features.
 * Current implementation: Find all the neighbor feature points to the predicted point.
 *                         Using NCC score to sort the results.
 * @brief GyroAidedTracker::FindAndSortNearNeighbor
 * @param mvvNearNeighbors [out]: order: if mbNCC == true, big to small; if mbNCC == false, small to big
 */
void GyroAidedTracker::FindAndSortNearNeighbor(const cv::Range& range, int level)
{
    for (int i = range.start; i < range.end; i++) {
        if (!mvStatus[i])
            continue;
        if (!mvvNearNeighbors[i].empty())   // the neighbors have been found in lower level (i.e., small search region)
            continue;

        std::vector<sMatch> vDMatchs;   // for ncc, [0]: the best correlation; for distance, [0]: the minimum distance
        std::stack<sMatch> stDMatches_1, stDMatches_2;

        // get pixel values and mean for pt_ref
        float mean_ref = 0.0f;
        std::vector<float> vValuesRef;
        for (int x = -mHalfPatchSize; x <= mHalfPatchSize; x++) {
            for (int y = -mHalfPatchSize; y <= mHalfPatchSize; y++) {
                float value_ref = GetPixelValue(mImgGrayRef, mvKeysRef[i].pt.x + x, mvKeysRef[i].pt.y + y);
                mean_ref += value_ref;
                vValuesRef.push_back(value_ref);
            }
        }
        mean_ref /= vValuesRef.size();

        float search_region_radiu = level * mRadiusForFindNearNeighbor;
        for (size_t j = 0; j < mvKeysCurUn.size(); j++) {
            cv::Point2f dpt = mvPtPredictUn[i] - mvKeysCurUn[j].pt;
            if (abs(dpt.x) > search_region_radiu || abs(dpt.y) > search_region_radiu)
                continue;

            // Euclidean distance between the predict point and detected keypoint in current frame.
            float distance = std::sqrt(dpt.x * dpt.x + dpt.y * dpt.y);

            // Correction between the keypoint in reference frame and the keypoint in current frame.
            float ncc = NCC(mHalfPatchSize, vValuesRef, mean_ref, mImgGrayCur, mvKeysCur[j].pt, mvAffineDeformationMatrix[i]);
            // float ncc = NCC(mHalfPatchSize, vValuesRef, mean_ref, mImgGrayCur, mvKeysCur[j].pt, cv::Mat()); // ncc withou warp (affine deformation matrix)

            sMatch match(i, j, distance, ncc, level);  // i: index of keypoint in reference frame; j: index of keypoint in current frame
            if (mbNCC) {    // use ncc to sort the matches
                while (!stDMatches_1.empty() && ncc < stDMatches_1.top().ncc) {
                    stDMatches_2.push(stDMatches_1.top());
                    stDMatches_1.pop();
                }
            }
            else {          // else, use distance to sort the matches
                while (!stDMatches_1.empty() && distance > stDMatches_1.top().distance) {
                    stDMatches_2.push(stDMatches_1.top());
                    stDMatches_1.pop();
                }
            }

            stDMatches_1.push(match);
            while (!stDMatches_2.empty()) {
                stDMatches_1.push(stDMatches_2.top());
                stDMatches_2.pop();
            }
        }

        while (!stDMatches_1.empty()) {
            vDMatchs.push_back(stDMatches_1.top());
            stDMatches_1.pop();
        }
        mvvNearNeighbors[i] = std::vector<sMatch>(vDMatchs.begin(), vDMatchs.end());
    }
}


/**
 * Search matches between keyoints in current frame and reference frame, using gyroscope integration
 * @brief GyroAidedTracker::SearchByGyroPredict
 * @return The number of matched fatures.
 */
int GyroAidedTracker::SearchByGyroPredict()
{
    IntegrateGyroMeasurements();

    Timer timer, timer_begin;

    /// Step 1: Predict features using gyroscope integrated rotation
    if (mType == GYRO_PREDICT)
        int n_predict = GyroPredictFeatures();
    else {
        if (mType == IMAGE_ONLY_OPTICAL_FLOW_CONSIDER_ILLUMINATION) {
            mbHasGyroPredictInitial = false;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = false;
        }
        else if (mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = false;
            mbConsiderAffineDeformation = false;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = false;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION_DEFORMATION) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = false;
        }
        else if(mType == GYRO_PREDICT_WITH_OPTICAL_FLOW_REFINED_CONSIDER_ILLUMINATION_DEFORMATION_REGULAR) {
            mbHasGyroPredictInitial = true;
            mbConsiderIllumination = true;
            mbConsiderAffineDeformation = true;
            mbRegularizationPenalty = true;
        }
        else {
            LOG(ERROR) << "Unsupport type!!! return -1;";
            return -1;
        }

        int n_predict = GyroPredictFeaturesAndOpticalFlowRefined();
    }

    mTimeFeaturePredict = timer.runTime_s();    timer.freshTimer();

    /// Step 2: For each predicted point in current frame, see which detected feature it belongs to.
    // Step 2.1: Find the neighbors for of predicted point. The neighbors are the detected features.
    //           radius: mRadiusForFindNearNeighbor (default: 4.0f)
    cv::parallel_for_(cv::Range(0, mN), std::bind(&GyroAidedTracker::FindAndSortNearNeighbor, this, placeholders::_1, 1));
    mTimeFindNearest = timer.runTime_s();   timer.freshTimer();

    // Step 2.2: See which detected features the predicted point belongs to.
    mvMatches.clear();
    MatchFeatures(mvMatches, mvvNearNeighbors);
    // LOG(WARNING) << "match: " << mvMatches.size();

    // If no enough matches, we wider the search region to find near neighbors and then match features again.
    if (mvMatches.size() < 100) {
        cv::parallel_for_(cv::Range(0, mN), std::bind(&GyroAidedTracker::FindAndSortNearNeighbor, this, placeholders::_1, 2));
        mvMatches.clear();
        MatchFeatures(mvMatches, mvvNearNeighbors);
    }

    // Step 3: calculate the predict error
    mvFlowsErrorUn = std::vector<cv::Point2f>(mN, cv::Point2f(0,0));
    for_each(mvMatches.begin(), mvMatches.end(), [&](sMatch _m) {
        cv::Point2f pt_predict = mvPtPredictUn[_m.queryIdx];
        cv::Point2f pt_detected = mvKeysCurUn[_m.trainIdx].pt;
        mvFlowsErrorUn[_m.queryIdx] = pt_detected - pt_predict;
    });

    mTimeFilterOut = timer.runTime_s();
    mTimeCost = timer_begin.runTime_s();

    return mvMatches.size();
}


/**
 * See which detected features the predicted point belongs to.
 * @brief GyroAidedTracker::MatchFeatures
 * @param vMatches
 * @param sFoundInCurPts
 * @param vvNearNeighbors
 */
void GyroAidedTracker::MatchFeatures(
        std::vector<sMatch> &vMatches,
        const vector<vector<sMatch>> &vvNearNeighbors)
{
    std::set<int> sFoundInCurPts;
    for (int i = 0; i < mN; i++) {
        if (vvNearNeighbors[i].empty()) // no neighbors
            continue;

        sMatch _m;
        if (mbNCC)  // Using the NCC score to choose the neighbor that is most similar to the reference keypoint (Default: true).
        {
            if (vvNearNeighbors[i][0].ncc > TH_NCC_HIGH)    // 0.85
                _m = vvNearNeighbors[i][0];
            else if (vvNearNeighbors[i].size() > 1) {
                if (vvNearNeighbors[i][0].ncc < TH_NCC_LOW) // 0.65
                    continue;

                sMatch _m0 = vvNearNeighbors[i][0], _m1 = vvNearNeighbors[i][1];
                if (_m1.ncc < _m0.ncc * TH_RATIO)    // If the two matches are not too similar
                    _m = _m0;
                else // Else the two matches are too similar, reject
                    continue;
            }
            else
                continue;
        }
        else    // Using the Euclidean distance to choose the neighbor that is closest to the predicted points on current frame.
        {
            if (vvNearNeighbors[i].size() == 1) { // only one neighbor
                _m = vvNearNeighbors[i][0];
            }
            else if (vvNearNeighbors[i].size() > 1) { // more than 2 neighbors - check how close they are
                if (vvNearNeighbors[i][0].distance < vvNearNeighbors[i][1].distance * TH_RATIO) // If not too close, chose the nearest one
                    _m = vvNearNeighbors[i][0];
                else                // Else too close, reject
                    continue;
            }
            else     // no neighbors
                continue;

        }

        ///////////////////////
        if (sFoundInCurPts.find(_m.trainIdx) == sFoundInCurPts.end()){ // not matched yet
            vMatches.push_back(_m);
            sFoundInCurPts.insert(_m.trainIdx);
        }
        else {
            // Wrong match: This keypoint in the current frame has been matched.
            // We delete the previous matches and prohibit the matches to this keypoint
            std::vector<sMatch>::iterator it = vMatches.begin();
            while (it != vMatches.end()) {
                if (it->trainIdx == _m.trainIdx)
                    it = vMatches.erase(it);
                else
                    it ++;
            }
        }
    }
}


/**
 * Search matches between keypoints in current frame and reference frame, using optical flow tracking (KLT)
 * @brief GyroAidedTracker::SearchByOpencvKLT
 * @return The number of matched features.
 */
int GyroAidedTracker::SearchByOpencvKLT()
{
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    if (mImgGrayRef.empty() || mImgGrayCur.empty()){
        LOG(ERROR) << "mImgGrayRef.empty() is " << mImgGrayRef.empty()
                   << ", mImgGrayCur.empty() is " << mImgGrayCur.empty();
        return -1;
    }

    vector<cv::Point2f> pt_ref_detected, pt_cur_detected; // detected ORB feature point
    for_each(mvKeysRef.begin(), mvKeysRef.end(), [&](cv::KeyPoint kp){
        pt_ref_detected.push_back(kp.pt);
    });
    for_each(mvKeysCur.begin(), mvKeysCur.end(), [&](cv::KeyPoint kp){
        pt_cur_detected.push_back(kp.pt);
    });

    // opencv optical flow tracking
    cv::Size winSize = cv::Size(2 * mHalfPatchSize + 1, 2 * mHalfPatchSize + 1);
    int maxLevel = 2;
    cv::TermCriteria criteria = cv::TermCriteria(TermCriteria::COUNT+TermCriteria::EPS, 30, 0.01);
    int flags = 0;
    double minEigThreshold = 1e-4;
    cv::calcOpticalFlowPyrLK(mImgGrayRef, mImgGrayCur, pt_ref_detected, mvPtPredict,
                             mvStatus, mvError, winSize, maxLevel, criteria, flags, minEigThreshold);

    // Step 1: filter out the points with high error
    vector<int> pt_cur_klt_find_index;
    vector<cv::Point2f> pt_cur_klt_find;
    for (size_t i = 0; i < mvStatus.size(); i++) {
        if(mvStatus[i] && (mvError[i]<12.0)) {
            // Keep the original index of the point in the optical flow array, for future use
            pt_cur_klt_find_index.push_back(i);
            // Keep the feature point itself
            pt_cur_klt_find.push_back(mvPtPredict[i]);
        }
        else {
            mvStatus[i] = 0;
        }
    }

    // Step 2: for each optical flow point in current frame, see which detected feature it belongs to
    cv::Mat pt_cur_klt_find_flat = cv::Mat(pt_cur_klt_find).reshape(1, pt_cur_klt_find.size()); // flatten array
    cv::Mat pt_cur_detected_flat = cv::Mat(pt_cur_detected).reshape(1, pt_cur_detected.size());

    // find the neraest ORB feature points to the optical flow point
    vector< vector<cv::DMatch> > nearest_neighbors;
    cv::BFMatcher matcher(cv::NORM_L2);
    float maxDistance = 4.0f;
    matcher.radiusMatch(pt_cur_klt_find_flat, pt_cur_detected_flat, nearest_neighbors, maxDistance); //2.0f

    // Step 3: Check that the found neighbors are unique
    //         (throw away neighbors that are too close to each other, as they may be confusing)
    std::set<int> found_in_cur_pts;
    double maxDisparity_1 = 0, sumDisparity_1 = 0;
    vector<cv::DMatch> vMatches_nearest_negihbor; // used for display
    for(size_t i = 0; i < nearest_neighbors.size(); i++){
        cv::DMatch _m;
        if(nearest_neighbors[i].size() == 1){   // only one neighbor
            _m = nearest_neighbors[i][0];
        }
        else if (nearest_neighbors[i].size() > 1){ // more than 2 neighbors - check how close they are
            double ratio = nearest_neighbors[i][0].distance / nearest_neighbors[i][1].distance;
            if(ratio < 0.7)     // not too close
                _m = nearest_neighbors[i][0];   // take the closest (first) one
            else                // too close, we cannot tell which is better
                continue;       // did not pass ratio tetst, throw away
        }
        else    // no neighbors
            continue;

        if(found_in_cur_pts.find(_m.trainIdx) == found_in_cur_pts.end()){
            // We should match it with the original indexing of the left point
            _m.queryIdx = pt_cur_klt_find_index[_m.queryIdx];
            mvMatches.push_back(sMatch(_m.queryIdx, _m.trainIdx, _m.distance));
            vMatches_nearest_negihbor.push_back(_m);

            // Calculate disparity
            cv::Point2f pt_ref = pt_ref_detected[_m.queryIdx];
            cv::Point2f pt_cur = pt_cur_detected[_m.trainIdx];
            //double disp = Vector2d(pt_ref.x - pt_cur.x, pt_ref.y - pt_cur.y).norm();
            double disp = std::sqrt((pt_ref.x - pt_cur.x) * (pt_ref.x - pt_cur.x) + (pt_ref.y - pt_cur.y) * (pt_ref.y - pt_cur.y));
            mvDisparities.push_back(disp);
            sumDisparity_1 += disp;
            maxDisparity_1 = disp > maxDisparity_1? disp : maxDisparity_1;

            found_in_cur_pts.insert(_m.trainIdx);
        }
    }

    // average disparity. used to filter out large disparity matching
    double avgDisparity_1 = sumDisparity_1/mvDisparities.size();

    // Step 4: filter large disparity
    std::vector<double>::iterator itDisp = mvDisparities.begin();
    std::vector<sMatch>::iterator itMatch = mvMatches.begin();
    double maxDisparity_2 = 0; double sumDisparity_2 = 0;
    double filterOutFactor = 1.5;
    double th = avgDisparity_1 * filterOutFactor;
    while (itDisp != mvDisparities.end()) {
        // filter out the matches whose disparity is larger than thDisparity*avgDisparity
        if(*itDisp > th){
            itDisp = mvDisparities.erase(itDisp);
            itMatch = mvMatches.erase(itMatch);
        }
        else {
            sumDisparity_2 += *itDisp;
            maxDisparity_2 = *itDisp > maxDisparity_2? *itDisp : maxDisparity_2;
            itDisp ++;
            itMatch ++;
        }
    }
    double avgDisparity_2 = sumDisparity_2/mvMatches.size();

    std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
    mTimeCost = std::chrono::duration_cast<std::chrono::duration<double> >(t2 - t1).count();

    return mvMatches.size();
}

