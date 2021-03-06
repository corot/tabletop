/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef TABLETOP_OBJECT_DETECTOR_H_
#define TABLETOP_OBJECT_DETECTOR_H_

// Author(s): Marius Muja and Matei Ciocarlie

#include <tabletop_object_detector/exhaustive_fit_detector.h>
#include <tabletop_object_detector/iterative_distance_fitter.h>

#include <string>
#include <future>

#include <opencv2/flann/flann.hpp>

namespace tabletop_object_detector
{
class TabletopObjectRecognizer
{
  private:
    //! The instance of the detector used for all detecting tasks
    ExhaustiveFitDetector<IterativeTranslationFitter> detector_;

    //! The threshold for merging two models that were fit very close to each other
    double fit_merge_threshold_;

    double getConfidence (double score) const
    {
      return (1.0 - (1.0 - score) * (1.0 - score));
    }
  public:
    //! Subscribes to and advertises topics; initializes fitter
    TabletopObjectRecognizer()
    {
     //XXX not needed, I think    detector_ = ExhaustiveFitDetector<IterativeTranslationFitter>();
      //initialize operational flags
      fit_merge_threshold_ = 0.02;
    }

    //! Empty stub
    ~TabletopObjectRecognizer()
    {
    }

    void
    clearObjects()
    {
      detector_.clearObjects();
    }

    void
    addObject(int model_id, const shape_msgs::Mesh & mesh)
    {
      detector_.addObject(model_id, mesh);
    }

    /** Structure used a return type for objectDetection */
    struct TabletopResult
    {
      geometry_msgs::Pose pose_;
      float confidence_;
      int object_id_;
      std::vector<cv::Vec3f> cloud_;
      size_t cloud_index_;
    };

    /*! Performs the detection on each of the clusters, and populates the returned message.
     */

    void
    objectDetection(std::vector<std::vector<cv::Vec3f> > &clusters, float confidence_cutoff,
                    bool perform_fit_merge, std::vector<TabletopResult > &results)
    {
      //do the model fitting part
      std::vector<size_t> cluster_model_indices;
      std::vector<std::vector<ModelFitInfo>> raw_fit_results(clusters.size());
      std::vector<std::future<std::vector<ModelFitInfo>>> future_results(clusters.size());
      std::vector<cv::flann::Index> search(clusters.size());
      cluster_model_indices.resize(clusters.size(), -1);
      int num_models = 1;

      for (size_t i = 0; i < clusters.size(); i++)
      {
        cluster_model_indices[i] = i;
        cv::Mat features = cv::Mat(clusters[i]).reshape(1);
#if CV_VERSION_MAJOR == 2 && CV_VERSION_MINOR == 4 && CV_VERSION_REVISION >= 12
        // That compiles but seems to crash on other versions
        search[i] = cv::flann::Index(features, cv::flann::KDTreeIndexParams());
#else
        search[i].build(features, cv::flann::KDTreeIndexParams());
#endif

        future_results[i] =
            std::async(std::launch::async,
                       &ExhaustiveFitDetector<IterativeTranslationFitter>::fitBestModels, &detector_,
                       std::ref(clusters[i]), std::max(1, num_models), std::ref(search[i]), confidence_cutoff);
      }

      for (size_t i = 0; i < clusters.size(); i++)
      {
        raw_fit_results[i] = future_results[i].get();
      }

      //merge models that were fit very close to each other
      if (perform_fit_merge)
      {
        size_t i = 0;
        while (i < clusters.size())
        {
          //if cluster i has already been merged continue
          if (cluster_model_indices[i] != (int) i || raw_fit_results.at(i).empty())
          {
            i++;
            continue;
          }

          size_t j;
          for (j = i + 1; j < clusters.size(); j++)
          {
            //if cluster j has already been merged continue
            if (cluster_model_indices[j] != (int) j)
              continue;
            //if there are no fits, merge based on cluster vs. fit
//            if (raw_fit_results.at(j).empty())
//            {
//              if (fitClusterDistance<typename pcl::PointCloud<PointType> >(raw_fit_results.at(i).at(0), *clusters[j])
//                  < fit_merge_threshold_)
//                break;
//              else
//                continue;
//            }
            //else merge based on fits
            if (!raw_fit_results.at(j).empty() && fitDistance(raw_fit_results.at(i).at(0), raw_fit_results.at(j).at(0)) < fit_merge_threshold_)
              break;
          }          
          if (j < clusters.size())
          {
            //merge cluster j into i
            clusters[i].insert(clusters[i].end(), clusters[j].begin(), clusters[j].end());
            //delete fits for cluster j so we ignore it from now on
            raw_fit_results.at(j).clear();
            //fits for cluster j now point at fit for cluster i
            cluster_model_indices[j] = i;
            //refit cluster i
            raw_fit_results.at(i) = detector_.fitBestModels(clusters[i], std::max(1, num_models), search[i], confidence_cutoff);
          }
          else
          {
            i++;
          }
        }
      }

      // Merge clusters together
      for (size_t i = 0; i < cluster_model_indices.size(); i++)
      {
        if ((cluster_model_indices[i] != int(i)) || (raw_fit_results[i].empty()))
          continue;

        double confidence = getConfidence (raw_fit_results[i][0].getScore());

        if (confidence < confidence_cutoff)
          continue;

        TabletopResult result;
        result.object_id_ = raw_fit_results[i][0].getModelId();
        result.pose_ = raw_fit_results[i][0].getPose();
        result.confidence_ = confidence;
        result.cloud_ = clusters[i];
        result.cloud_index_ = i;

        results.push_back(result);
      }
    }

    //-------------------- Misc -------------------

    //! Helper function that returns the distance along the plane between two fit models
    double
    fitDistance(const ModelFitInfo &m1, const ModelFitInfo &m2)
    {
      double dx = m1.getPose().position.x - m2.getPose().position.x;
      double dy = m1.getPose().position.y - m2.getPose().position.y;
      double d = dx * dx + dy * dy;
      return sqrt(d);
    }

    template<class PointCloudType>
    double
    fitClusterDistance(const ModelFitInfo &m, const PointCloudType &cluster)
    {
      double dist = 100.0 * 100.0;
      double mx = m.getPose().position.x;
      double my = m.getPose().position.y;
      for (size_t i = 0; i < cluster.points.size(); i++)
      {
        double dx = cluster.points[i].x - mx;
        double dy = cluster.points[i].y - my;
        double d = dx * dx + dy * dy;
        dist = std::min(d, dist);
      }
      return sqrt(dist);
    }
  };
}

#endif /* TABLETOP_OBJECT_DETECTOR_H_ */
