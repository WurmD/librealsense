// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include <librealsense2/utilities/easylogging/easyloggingpp.h>

#include <realdds/dds-device-broadcaster.h>
#include <realdds/dds-device-server.h>
#include <realdds/dds-stream-server.h>
#include <realdds/dds-participant.h>
#include <realdds/dds-utilities.h>
#include <realdds/dds-log-consumer.h>
#include <realdds/topics/device-info/device-info-msg.h>
#include <realdds/topics/flexible/flexible-msg.h>

#include <fastrtps/types/TypesBase.h>
#include <fastdds/dds/log/Log.hpp>

#include "lrs-device-watcher.h"
#include "lrs-device-controller.h"

#include <tclap/CmdLine.h>
#include <tclap/ValueArg.h>

#include <string>
#include <iostream>
#include <map>
#include <unordered_set>

using namespace TCLAP;
using namespace realdds;

#define NAME2SERVER( X )                                                                                               \
    if( server )                                                                                                       \
    {                                                                                                                  \
        if( strcmp( server->type_string(), #X ) )                                                                      \
        {                                                                                                              \
            LOG_ERROR( #X " profile type on a stream '" << stream_name << "' that already has type "                   \
                                                        << server->type_string() );                                    \
            return;                                                                                                    \
        }                                                                                                              \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        server = std::make_shared< realdds::dds_##X##_stream_server >( stream_name, sensor_name );                     \
    }                                                                                                                  \
    break


std::vector< std::shared_ptr< realdds::dds_stream_server > > get_supported_streams( rs2::device dev )
{
    std::map< std::string, realdds::dds_stream_profiles > name_to_profiles;
    std::map< std::string, int > name_to_default_profile;
    std::map< std::string, std::string > name_to_sensor;
    std::map< std::string, std::shared_ptr< realdds::dds_stream_server > > name_to_server;
    for( auto sensor : dev.query_sensors() )
    {
        std::string const sensor_name = sensor.get_info( RS2_CAMERA_INFO_NAME );
        auto stream_profiles = sensor.get_stream_profiles();
        std::for_each( stream_profiles.begin(), stream_profiles.end(), [&]( const rs2::stream_profile & sp ) {
            std::string stream_name = sp.stream_name();
            auto & server = name_to_server[stream_name];
            switch( sp.stream_type() )
            {
            case RS2_STREAM_DEPTH: NAME2SERVER( depth );
            case RS2_STREAM_INFRARED: NAME2SERVER( ir );
            case RS2_STREAM_COLOR: NAME2SERVER( color );
            case RS2_STREAM_FISHEYE: NAME2SERVER( fisheye );
            case RS2_STREAM_CONFIDENCE: NAME2SERVER( confidence );
            case RS2_STREAM_ACCEL: NAME2SERVER( accel );
            case RS2_STREAM_GYRO: NAME2SERVER( gyro );
            case RS2_STREAM_POSE: NAME2SERVER( pose );
            default:
                LOG_ERROR( "unsupported stream type " << sp.stream_type() );
                return;
            }
            name_to_sensor[stream_name] = sensor_name;
            auto & profiles = name_to_profiles[stream_name];
            std::shared_ptr< realdds::dds_stream_profile > profile;
            if( sp.is< rs2::video_stream_profile >() )
            {
                auto const & vsp = sp.as< rs2::video_stream_profile >();
                profile = std::make_shared< realdds::dds_video_stream_profile >(
                    static_cast<int16_t>( vsp.fps() ),
                    realdds::dds_stream_format::from_rs2( vsp.format() ),
                    static_cast< uint16_t >( vsp.width() ),
                    static_cast< int16_t >( vsp.height() ) );
            }
            else if( sp.is< rs2::motion_stream_profile >() )
            {
                const auto & msp = sp.as< rs2::motion_stream_profile >();
                profile = std::make_shared< realdds::dds_motion_stream_profile >(
                    static_cast< int16_t >( msp.fps() ),
                    realdds::dds_stream_format::from_rs2( msp.format() ) );
            }
            else
            {
                LOG_ERROR( "unknown profile type of uid " << sp.unique_id() );
                return;
            }
            if( sp.is_default() )
                name_to_default_profile[stream_name] = static_cast< int >( profiles.size() );
            profiles.push_back( profile );
            LOG_DEBUG( stream_name << ": " << profile->to_string() );
        } );
    }
    std::vector< std::shared_ptr< realdds::dds_stream_server > > servers;
    for( auto & it : name_to_profiles )
    {
        auto const & stream_name = it.first;
        
        int default_profile_index = 0;
        auto default_profile_it = name_to_default_profile.find( stream_name );
        if( default_profile_it != name_to_default_profile.end() )
            default_profile_index = default_profile_it->second;

        auto const& profiles = it.second;
        if( profiles.empty() )
        {
            LOG_ERROR( "ignoring stream '" << stream_name << "' with no profiles" );
            continue;
        }
        std::string const & sensor_name = name_to_sensor[stream_name];
        auto server = name_to_server[stream_name];
        if( ! server )
        {
            LOG_ERROR( "ignoring stream '" << stream_name << "' with no server" );
            continue;
        }
        server->init_profiles( profiles, default_profile_index );
        servers.push_back( server );
    }
    return servers;
}

#undef NAME2SERVER


std::string get_topic_root( topics::device_info const & dev_info )
{
    // Build device root path (we use a device model only name like DXXX)
    // example: /realsense/D435/11223344
    constexpr char const * DEVICE_NAME_PREFIX = "Intel RealSense ";
    constexpr size_t DEVICE_NAME_PREFIX_CCH = 16;
    // We don't need the prefix in the path
    std::string model_name = dev_info.name;
    if ( model_name.length() > DEVICE_NAME_PREFIX_CCH
       && 0 == strncmp( model_name.data(), DEVICE_NAME_PREFIX, DEVICE_NAME_PREFIX_CCH ) )
    {
        model_name.erase( 0, DEVICE_NAME_PREFIX_CCH );
    }
    constexpr char const * RS_ROOT = "realsense/";
    return RS_ROOT + model_name + '/' + dev_info.serial;
}


topics::device_info rs2_device_to_info( rs2::device const & dev )
{
    topics::device_info dev_info;
    dev_info.name = dev.get_info( RS2_CAMERA_INFO_NAME );
    dev_info.serial = dev.get_info( RS2_CAMERA_INFO_SERIAL_NUMBER );
    dev_info.product_line = dev.get_info( RS2_CAMERA_INFO_PRODUCT_LINE );
    dev_info.locked = ( dev.get_info( RS2_CAMERA_INFO_CAMERA_LOCKED ) == "YES" );

    // Build device topic root path
    dev_info.topic_root = get_topic_root( dev_info );
    return dev_info;
}


int main( int argc, char * argv[] )
try
{
    dds_domain_id domain = 0;
    CmdLine cmd( "librealsense rs-dds-server tool, use CTRL + C to stop..", ' ' );
    ValueArg< dds_domain_id > domain_arg( "d",
                                          "domain",
                                          "Select domain ID to listen on",
                                          false,
                                          0,
                                          "0-232" );
    SwitchArg debug_arg( "", "debug", "Enable debug logging", false );

    cmd.add( domain_arg );
    cmd.add( debug_arg );
    cmd.parse( argc, argv );

    // Configure the same logger as librealsense
    el::Configurations defaultConf;
    defaultConf.setToDefault();
    defaultConf.setGlobally( el::ConfigurationType::ToStandardOutput, debug_arg.isSet() ? "true" : "false" );
    if( ! debug_arg.isSet() )
        defaultConf.set( el::Level::Error, el::ConfigurationType::ToStandardOutput, "true" );
    defaultConf.setGlobally( el::ConfigurationType::Format, "-%levshort- %datetime{%H:%m:%s.%g} %msg (%fbase:%line [%thread])" );
    el::Loggers::reconfigureLogger( "librealsense", defaultConf );

    // Intercept DDS messages and redirect them to our own logging mechanism
    eprosima::fastdds::dds::Log::ClearConsumers();
    eprosima::fastdds::dds::Log::RegisterConsumer( realdds::log_consumer::create() );

    if( debug_arg.isSet() )
    {
        rs2::log_to_console( RS2_LOG_SEVERITY_DEBUG );
        eprosima::fastdds::dds::Log::SetVerbosity( eprosima::fastdds::dds::Log::Info );
    }
    else
    {
        rs2::log_to_console( RS2_LOG_SEVERITY_ERROR );
        eprosima::fastdds::dds::Log::SetVerbosity( eprosima::fastdds::dds::Log::Error );
    }

    if( domain_arg.isSet() )
    {
        domain = domain_arg.getValue();
        if( domain > 232 )
        {
            std::cerr << "Invalid domain value, enter a value in the range [0, 232]" << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cout << "Starting RS DDS Server.." << std::endl;

    // Create a DDS publisher
    auto participant = std::make_shared< dds_participant >();
    participant->init( domain, "rs-dds-server" );

    // Run the DDS device broadcaster
    dds_device_broadcaster broadcaster( participant );
    if( !broadcaster.run() )
    {
        std::cerr << "Failure running the DDS Device Broadcaster" << std::endl;
        return EXIT_FAILURE;
    }

    struct device_handler
    {
        topics::device_info info;
        std::shared_ptr< dds_device_server > server;
        std::shared_ptr< tools::lrs_device_controller > controller;
    };
    std::map< rs2::device, device_handler > device_handlers_list;
    
    std::cout << "Start listening to RS devices.." << std::endl;

    // Create a RealSense context
    rs2::context ctx( "{"
        "\"dds-discovery\" : false"
        "}" );

    // Run the LRS device watcher
    tools::lrs_device_watcher dev_watcher( ctx );
    dev_watcher.run(
        // Handle a device connection
        [&]( rs2::device dev ) {

            auto dev_info = rs2_device_to_info( dev );

            // Broadcast the new connected device to all listeners
            broadcaster.add_device( dev_info );

            // Create a dds-device-server for this device
            auto dds_device_server
                = std::make_shared< realdds::dds_device_server >( participant, dev_info.topic_root );
 
            // Create a lrs_device_manager for this device
            std::shared_ptr< tools::lrs_device_controller > lrs_device_controller
                = std::make_shared< tools::lrs_device_controller >( dev, dds_device_server );

            // Create a supported streams list for initializing the relevant DDS topics
            auto supported_streams = get_supported_streams( dev );

            // Initialize the DDS device server with the supported streams
            dds_device_server->init( supported_streams );

            // Keep a pair of device controller and server per RS device
            device_handlers_list.emplace( dev, device_handler{ dev_info, dds_device_server, lrs_device_controller } );
        },
        // Handle a device disconnection
        [&]( rs2::device dev ) {
            // Remove the dds-server for this device
            auto const & handler = device_handlers_list.at( dev );

            // Remove this device from the DDS device broadcaster
            broadcaster.remove_device( handler.info );

            device_handlers_list.erase( dev );
        } );

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), 0);// Pend until CTRL + C is pressed 

    std::cout << "Shutting down rs-dds-server..." << std::endl;

    return EXIT_SUCCESS;
}
catch( const rs2::error & e )
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args()
              << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch( const std::exception & e )
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}