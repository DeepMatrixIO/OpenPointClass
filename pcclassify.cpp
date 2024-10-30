#include "constants.hpp"
#include "point_io.hpp"
#include "features.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <memory>
#include <iomanip>  // for std::setprecision
#include "vendor/cxxopts.hpp"

int main(int argc, char **argv) {
    cxxopts::Options options("pcclassify", "Classifies a point cloud using a precomputed model");
    options.add_options()
        ("i,input", "Input point cloud", cxxopts::value<std::string>())
        ("o,output", "Output point cloud", cxxopts::value<std::string>())
        ("h,help", "Print usage")
        ;
    options.parse_positional({ "input", "output"});
    options.positional_help("[input point cloud] [output point cloud]");
    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    }
    catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << options.help() << std::endl;
        return EXIT_FAILURE;
    }

    bool showHelp = false;

    if (result.count("help") || !result.count("input") || !result.count("output")) showHelp = true;

    if (showHelp) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    try {
        // Read points
        const auto inputFile = result["input"].as<std::string>();
        const auto outputFile = result["output"].as<std::string>();
        std::vector<int> skip = {};
        if (result.count("skip")) skip = result["skip"].as<std::vector<int>>();

        const double startResolution = 0.005;
        const double radius = 0.75;
        const int numScales = 10;

        const auto pointSet = readPointSet(inputFile);

        std::cout << "Starting resolution: " << startResolution << std::endl;

        const auto features = getFeatures(computeScales(numScales, pointSet, startResolution, radius));
        std::cout << "Features: " << features.size() << std::endl;

        std::ofstream csvFile(outputFile);
        if (!csvFile.is_open()) {
            throw std::runtime_error("Could not open output file: " + outputFile);
        }

        csvFile << std::fixed << std::setprecision(6);

        for (size_t i = 0; i < features.size(); ++i) {
            csvFile << features[i]->getName();
            if (i < features.size() - 1) csvFile << ",";
        }
        csvFile << "\n";

        const size_t numPoints = pointSet->points.size();
        for (size_t pointIdx = 0; pointIdx < numPoints; ++pointIdx) {
            for (size_t featureIdx = 0; featureIdx < features.size(); ++featureIdx) {
                csvFile << features[featureIdx]->getValue(pointIdx);
                if (featureIdx < features.size() - 1) csvFile << ",";
            }
            csvFile << "\n";
        }

        csvFile.close();

        std::cout << "Features saved to: " << outputFile << std::endl;
        std::cout << "Number of points: " << numPoints << std::endl;
        std::cout << "Number of features: " << features.size() << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}