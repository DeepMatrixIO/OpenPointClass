#include "scale.hpp"

Scale::Scale(const size_t id, PointSet *pSet, const double resolution, const int kNeighbors, const double radius) :
    id(id), pSet(pSet), scaledSet(new PointSet()), resolution(resolution), kNeighbors(kNeighbors), radius(radius) {
    if (pSet && !pSet->points.empty()) {
        x0 = pSet->points[0][0];
        y0 = pSet->points[0][1];
        z0 = pSet->points[0][2];
        scaleCoordinates = {x0, y0, z0};
    }
}

void Scale::updateScaleCoordinates() {
    if (scaledSet && !scaledSet->points.empty()) {
        x0 = scaledSet->points[0][0];
        y0 = scaledSet->points[0][1];
        z0 = scaledSet->points[0][2];
        scaleCoordinates = {x0, y0, z0};
    }
}

void Scale::init() {
    #pragma omp critical
    {
        std::cout << "Init scale " << id << " at " << resolution << " ..." << std::endl;
    }
    if (id == 0) {
        pSet->pointMap.resize(pSet->count());
    }
    else if (id > 0) {
        eigenValues.resize(pSet->count());
        eigenVectors.resize(pSet->count());
        orderAxis.resize(pSet->count());
        heightMin.resize(pSet->count());
        heightMax.resize(pSet->count());

        if (id == 1) {
            avgHsv.resize(pSet->count());
        }
    }

    computeScaledSet();
    updateScaleCoordinates();
}

void Scale::build() {
    #pragma omp critical
    {
        std::cout << "Building scale " << id << " (" << scaledSet->count() << " points) ..." << std::endl;
    }

    #pragma omp parallel
    {
        const KdTree *index = scaledSet->getIndex<KdTree>();
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver;
        std::vector<size_t> neighborIds(kNeighbors);
        std::vector<float> sqrDists(kNeighbors);

        #pragma omp for
        for (long long int idx = 0; idx < pSet->count(); idx++) {
            index->knnSearch(pSet->points[idx].data(), kNeighbors, neighborIds.data(), sqrDists.data());
            Eigen::Vector3f medoid = computeMedoid(neighborIds);
            Eigen::Matrix3d covariance = computeCovariance(neighborIds, medoid);
            solver.computeDirect(covariance);
            Eigen::Vector3d ev = solver.eigenvalues();
            for (size_t i = 0; i < 3; i++) ev[i] = std::max(ev[i], 0.0);

            double sum = ev[0] + ev[1] + ev[2];
            eigenValues[idx] = (ev / sum).cast<float>(); // sum-normalized
            eigenVectors[idx] = solver.eigenvectors().cast<float>();

            // std::cout <<  "==Covariance==" << std::endl <<
            //     covariance << std::endl;
            // std::cout  << "==Medoid==" << std::endl <<
            //     medoid << std::endl;
            // std::cout <<  "==Eigenvalues==" << std::endl <<
            //     eigenValues[idx] << std::endl;
            // std::cout <<  "==Eigenvectors==" << std::endl <<
            //     eigenVectors[idx] << std::endl;
            // exit(1);

            // lambda1 = eigenValues[idx][2]
            // lambda3 = eigenValues[idx][0]

            // e1 = eigenVectors[idx].col(2)
            // e3 = eigenVectors[idx].col(0)
            orderAxis[idx](0, 0) = 0.f;
            orderAxis[idx](1, 0) = 0.f;
            orderAxis[idx](0, 1) = 0.f;
            orderAxis[idx](1, 1) = 0.f;

            heightMin[idx] = std::numeric_limits<float>::max();
            heightMax[idx] = std::numeric_limits<float>::min();

            for (size_t const &i : neighborIds) {
                Eigen::Vector3f p(scaledSet->points[i][0],
                    scaledSet->points[i][1],
                    scaledSet->points[i][2]);
                Eigen::Vector3f n = (p - medoid);
                const float v00 = n.dot(eigenVectors[idx].col(2));
                const float v01 = n.dot(eigenVectors[idx].col(1));
                orderAxis[idx](0, 0) += v00;
                orderAxis[idx](0, 1) += v01;
                orderAxis[idx](1, 0) += v00 * v00;
                orderAxis[idx](1, 1) += v01 * v01;

                if (p[2] > heightMax[idx]) heightMax[idx] = p[2];
                if (p[2] < heightMin[idx]) heightMin[idx] = p[2];
            }
        }

        if (id == 1) {
            std::vector<nanoflann::ResultItem<size_t, float>> radiusMatches;

            #pragma omp for
            for (long long int idx = 0; idx < pSet->count(); idx++) {
                const size_t numMatches = index->radiusSearch(pSet->points[idx].data(), static_cast<float>(radius), radiusMatches);
                avgHsv[idx] = { 0.f, 0.f, 0.f };

                for (size_t i = 0; i < numMatches; i++) {
                    const size_t nIdx = radiusMatches[i].first;
                    auto hsv = rgb2hsv(scaledSet->colors[nIdx][0],
                        scaledSet->colors[nIdx][1],
                        scaledSet->colors[nIdx][2]);
                    for (size_t j = 0; j < 3; j++)
                        avgHsv[idx][j] += hsv[j];
                }

                if (numMatches > 0) {
                    for (size_t j = 0; j < 3; j++)
                        avgHsv[idx][j] /= numMatches;
                }
            }
        }

    }
}

void Scale::computeScaledSet() {
    if (scaledSet->points.empty()) {
        const bool trackPoints = id == 0;

        // Voxel centroid nearest neighbor
        // Roughly from https://raw.githubusercontent.com/PDAL/PDAL/master/filters/VoxelCentroidNearestNeighborFilter.cpp
        const double x0 = pSet->points[0][0];
        const double y0 = pSet->points[0][1];
        const double z0 = pSet->points[0][2];

        typedef std::make_signed_t<std::size_t> ssize_t;

        // Make an initial pass through the input to index indices by
        // row, column, and depth.
        std::map<std::tuple<ssize_t, ssize_t, ssize_t>, std::vector<size_t> > populated_voxel_ids;

        for (size_t id = 0; id < pSet->count(); id++) {
            populated_voxel_ids[std::make_tuple(
                static_cast<ssize_t>((pSet->points[id][0] - y0) / resolution),  // r
                static_cast<ssize_t>((pSet->points[id][1] - x0) / resolution),  // c
                static_cast<ssize_t>((pSet->points[id][2] - z0) / resolution) // d
            )].push_back(id);
        }

        // Make a second pass through the populated voxels to compute the voxel
        // centroid and to find its nearest neighbor.
        scaledSet->points.clear();
        scaledSet->colors.clear();

        for (auto const &t : populated_voxel_ids) {
            if (t.second.size() == 1) {
                // If there is only one point in the voxel, simply append it.
                scaledSet->appendPoint(*pSet, t.second[0]);
                if (trackPoints) scaledSet->trackPoint(*pSet, t.second[0]);
            }
            else if (t.second.size() == 2) {
                // Else if there are only two, they are equidistant to the
                // centroid, so append the one closest to voxel center.

                // Compute voxel center.
                const double y_center = y0 + (std::get<0>(t.first) + 0.5) * resolution;
                const double x_center = x0 + (std::get<1>(t.first) + 0.5) * resolution;
                const double z_center = z0 + (std::get<2>(t.first) + 0.5) * resolution;

                // Compute distance from first point to voxel center.
                const double x1 = pSet->points[t.second[0]][0];
                const double y1 = pSet->points[t.second[0]][1];
                const double z1 = pSet->points[t.second[0]][2];
                const double d1 = std::pow<double>(x_center - x1, 2) + std::pow<double>(y_center - y1, 2) + std::pow<double>(z_center - z1, 2);
                // Compute distance from second point to voxel center.
                const double x2 = pSet->points[t.second[1]][0];
                const double y2 = pSet->points[t.second[1]][1];
                const double z2 = pSet->points[t.second[1]][2];
                const double d2 = std::pow<double>(x_center - x2, 2) + std::pow<double>(y_center - y2, 2) + std::pow<double>(z_center - z2, 2);

                // Append the closer of the two.
                if (d1 < d2) scaledSet->appendPoint(*pSet, t.second[0]);
                else scaledSet->appendPoint(*pSet, t.second[1]);

                if (trackPoints) {
                    scaledSet->trackPoint(*pSet, t.second[0]);
                    scaledSet->trackPoint(*pSet, t.second[1]);
                }
            }
            else {
                // Else there are more than two neighbors, so choose the one
                // closest to the centroid.

                // Compute the centroid.
                Eigen::Vector3f centroid = computeCentroid(t.second);

                // Compute distance from each point in the voxel to the centroid,
                // retaining only the closest.
                size_t pmin = 0;
                double dmin((std::numeric_limits<double>::max)());
                for (auto const &p : t.second) {
                    const double sqr_dist = std::pow<double>(centroid[0] - pSet->points[p][0], 2) +
                        std::pow<double>(centroid[1] - pSet->points[p][1], 2) +
                        std::pow<double>(centroid[2] - pSet->points[p][2], 2);
                    if (sqr_dist < dmin) {
                        dmin = sqr_dist;
                        pmin = p;
                    }
                }

                scaledSet->appendPoint(*pSet, pmin);

                if (trackPoints) {
                    for (auto const &p : t.second) {
                        scaledSet->trackPoint(*pSet, p);
                    }
                }
            }
        }
    }

    if (id > 0) scaledSet->buildIndex<KdTree>();
}

void Scale::save(const std::string &filename) {
    savePointSet(*scaledSet, filename);
}

Eigen::Matrix3d Scale::computeCovariance(const std::vector<size_t> &neighborIds, const Eigen::Vector3f &medoid) {
    Eigen::MatrixXd A(3, neighborIds.size());
    size_t k = 0;

    for (size_t const &i : neighborIds) {
        A(0, k) = scaledSet->points[i][0] - medoid[0];
        A(1, k) = scaledSet->points[i][1] - medoid[1];
        A(2, k) = scaledSet->points[i][2] - medoid[2];
        k++;
    }

    return A * A.transpose() / (neighborIds.size() - 1);
}

Eigen::Vector3f Scale::computeMedoid(const std::vector<size_t> &neighborIds) {
    float mx, my, mz;
    mx = my = mz = 0.0;
    float minDist = std::numeric_limits<float>::max();
    for (size_t const &i : neighborIds) {
        float sum = 0.0;
        const float xi = scaledSet->points[i][0];
        const float yi = scaledSet->points[i][1];
        const float zi = scaledSet->points[i][2];

        for (size_t const &j : neighborIds) {
            sum += std::pow<double>(xi - scaledSet->points[j][0], 2) +
                std::pow<double>(yi - scaledSet->points[j][1], 2) +
                std::pow<double>(zi - scaledSet->points[j][2], 2);
        }

        if (sum < minDist) {
            mx = xi;
            my = yi;
            mz = zi;
            minDist = sum;
        }
    }

    Eigen::Vector3f medoid;
    medoid << mx, my, mz;

    return medoid;
}

Eigen::Vector3f Scale::computeCentroid(const std::vector<size_t> &pointIds) {
    float mx, my, mz;
    mx = my = mz = 0.0;
    size_t n = 0;
    for (auto const &j : pointIds) {
        auto update = [&n](const float value, const float average) {
            const float delta = value - average;
            const float delta_n = delta / n;
            return average + delta_n;
        };
        n++;
        mx = update(pSet->points[j][0], mx);
        my = update(pSet->points[j][1], my);
        mz = update(pSet->points[j][2], mz);
    }

    Eigen::Vector3f centroid;
    centroid << mx, my, mz;

    return centroid;
}

std::vector<Scale *> computeScales(size_t numScales, PointSet *pSet, double startResolution, double radius) {
    std::vector<Scale *> scales(numScales, nullptr);

    auto *base = new Scale(0, pSet, startResolution * std::pow<double>(2.0, 0), 10, radius);
    base->init();
    pSet->base = base->scaledSet;

    for (size_t i = 0; i < numScales; i++) {
        scales[i] = new Scale(i + 1, base->scaledSet, startResolution * std::pow<double>(2.0, i), 10, radius);
    }

    scales[0]->scaledSet = base->scaledSet;
    scales[0]->updateScaleCoordinates();

    #pragma omp parallel for
    for (int i = 0; i < numScales; i++) {
        scales[i]->init();
    }

    for (int i = 0; i < numScales; i++) {
        scales[i]->build();

        #pragma omp critical
        {
            std::cout << "Scale " << i + 1 << " coordinates: "
                     << "x=" << scales[i]->x0
                     << ", y=" << scales[i]->y0
                     << ", z=" << scales[i]->z0 << std::endl;
        }
    }

    return scales;
}

