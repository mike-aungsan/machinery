#include "machinery/io/IoVideo.h"

namespace kerberos
{
    void IoVideo::setup(const StringMap & settings)
    {
        Io::setup(settings);
        
        m_writer = 0;
        m_recording = false;
        pthread_mutex_init(&m_lock, NULL);
        pthread_mutex_init(&m_time_lock, NULL);
        pthread_mutex_init(&m_capture_lock, NULL);
        pthread_mutex_init(&m_write_lock, NULL);
        startRetrieveThread();

        // ----------------------------------------
        // If privacy mode is enabled, we calculate
        // a mask to remove the public area.
        
        m_privacy = (settings.at("ios.Video.privacy") == "true");

        if(m_privacy)
        {
            // --------------------------------
            // Parse coordinates from config file
            //  - x,y|x,y|x,y|... => add to vectory as Point2f
            
            std::vector<cv::Point2f> coor;
            
            std::vector<std::string> coordinates;
            helper::tokenize(settings.at("expositors.Hull.region"), coordinates, "|");
            
            for(int i = 0; i < coordinates.size(); i++)
            {
                std::vector<std::string> fromAndTo;
                helper::tokenize(coordinates[i], fromAndTo, ",");
                int from = std::atoi(fromAndTo[0].c_str());
                int to = std::atoi(fromAndTo[1].c_str());
                Point2f p(from ,to);
                coor.push_back(p);
            }
            
            // -------------------------------
            // Get width and height of image
            
            Image preview = m_capture->retrieve();
            int captureWidth = preview.getColumns();
            int captureHeight = preview.getRows();

            // --------------------------------
            // Calculate points in hull
            
            PointVector points;
            points.clear();
            for(int j = 0; j < captureHeight; j++)
            {
                for(int i = 0; i < captureWidth; i++)
                {
                    cv::Point2f p(i,j);
                    if(cv::pointPolygonTest(coor, p, false) >= 0)
                    {
                        points.push_back(p);
                    }
                }
            }

            m_mask.createMask(captureWidth, captureHeight, points);
        }
        
        // --------------------------
        // Get name from instance
        
        std::string instanceName = settings.at("name");
        setInstanceName(instanceName);

        m_fps = std::atoi(settings.at("ios.Video.fps").c_str());
        m_width = std::atoi(settings.at("capture.width").c_str());
        m_height = std::atoi(settings.at("capture.height").c_str());
        m_recordingTimeAfter = std::atoi(settings.at("ios.Video.recordAfter").c_str()); // in seconds
        m_extension = settings.at("ios.Video.extension");
        std::string codec = settings.at("ios.Video.codec");
        
        m_publicKey = settings.at("clouds.S3.publicKey");
        m_privateKey = settings.at("clouds.S3.privateKey");
        
        m_createSymbol = false;
        if(m_privateKey != "" && m_publicKey != "")
        {
            m_createSymbol = true;
        }
        
        if(codec == "h264")
        {
            m_codec = 0x00000021;//CV_FOURCC('H','2','6','4');
        }
        else
        {
            m_codec = -1;
        }

        // --------------------------
        // Check if need to draw timestamp
        
        bool drawTimestamp = (settings.at("ios.Video.markWithTimestamp") == "true");
        setDrawTimestamp(drawTimestamp);
        cv::Scalar color = getColor(settings.at("ios.Video.timestampColor"));
        setTimestampColor(color);
        
        std::string timezone = settings.at("timezone");
        std::replace(timezone.begin(), timezone.end(), '-', '/');
        std::replace(timezone.begin(), timezone.end(), '$', '_');
        setTimezone(timezone);
        
        // -------------------------------------------------------------
        // Filemanager is mapped to a directory and is used by an image
        // to save to the correct directory.
        
        setFileFormat(settings.at("ios.Video.fileFormat"));
        m_directory = settings.at("ios.Video.directory");
    }
    
    cv::Scalar IoVideo::getColor(const std::string name)
    {
        std::map<std::string, cv::Scalar> m_colors;
        
        m_colors["white"] = cv::Scalar(255,255,255);
        m_colors["black"] = cv::Scalar(0,0,0);
        m_colors["red"] = cv::Scalar(0,0,255);
        m_colors["green"] = cv::Scalar(0,255,0);
        m_colors["blue"] = cv::Scalar(255,0,0);
        
        if(name == "none")
        {
            return m_colors.at("white");
        }
        else
        {
            return m_colors.at(name);
        }
    }

    std::string IoVideo::buildPath(std::string pathToVideo, JSON & data)
    {
         // ------------------------------------------
        // Stringify data object: build image path
        // with data information.
        
        static const std::string kTypeNames[] = { "Null", "False", "True", "Object", "Array", "String", "Number" };
        for (JSONValue::ConstMemberIterator itr = data.MemberBegin(); itr != data.MemberEnd(); ++itr)
        {
            std::string name = itr->name.GetString();
            std::string type = kTypeNames[itr->value.GetType()];
            
            if(type == "String")
            {
                std::string value = itr->value.GetString();
                kerberos::helper::replace(pathToVideo, name, value);
            }
            else if(type == "Number")
            {
                std::string value = kerberos::helper::to_string(itr->value.GetInt());
                kerberos::helper::replace(pathToVideo, name, value);
            }
            else if(type == "Array")
            {
                std::string arrayString = "";
                for (JSONValue::ConstValueIterator itr2 = itr->value.Begin(); itr2 != itr->value.End(); ++itr2)
                {
                    type = kTypeNames[itr2->GetType()];
                    
                    if(type == "String")
                    {
                        arrayString += itr2->GetString();
                    }
                    else if(type == "Number")
                    {
                       arrayString += kerberos::helper::to_string(itr2->GetInt());
                    }
                    
                    arrayString += "-";
                }
                kerberos::helper::replace(pathToVideo, name, arrayString.substr(0, arrayString.size()-1));
            }
        }
        // -----------------------------------------------
        // Get timestamp, microseconds, random token, and instance name
        
        std::string instanceName = getInstanceName();
        kerberos::helper::replace(pathToVideo, "instanceName", instanceName);

        std::string timestamp = kerberos::helper::getTimestamp();
        kerberos::helper::replace(pathToVideo, "timestamp", timestamp);

        std::string microseconds = kerberos::helper::getMicroseconds();
        std::string size = kerberos::helper::to_string((int)microseconds.length());
        kerberos::helper::replace(pathToVideo, "microseconds", size + "-" + microseconds);

        std::string token = kerberos::helper::to_string(rand()%1000);
        kerberos::helper::replace(pathToVideo, "token", token);

        return pathToVideo;
    }

    void IoVideo::fire(JSON & data)
    {
        m_recording = true;

        // ----------------------------------------------------------
        // If a video is recording, and a new detection is coming in,
        // we'll reset the timer. So the video is expaned.
        // timer = ...

        pthread_mutex_lock(&m_time_lock);
        m_timeStartedRecording = (double) (cv::getTickCount() / cv::getTickFrequency());
        pthread_mutex_unlock(&m_time_lock);
        
        // -----------------------------------------------------
        // Check if already recording, if not start a new video
        
        if(m_capture && m_writer == 0)
        {
            // ----------------------------------------
            // The naming convention that will be used
            // for the image.
            
            std::string pathToVideo = getFileFormat();
            m_fileName = buildPath(pathToVideo, data) + "." + m_extension;
            Image image = m_capture->retrieve();
            
            m_writer = new cv::VideoWriter();
            m_writer->open(m_directory + m_fileName, m_codec, m_fps, cv::Size(image.getColumns(), image.getRows()));
            
            startRecordThread();
        }
    }

    void IoVideo::disableCapture()
    {
        pthread_mutex_lock(&m_write_lock);
        pthread_mutex_lock(&m_capture_lock);
        
        m_capture = 0; // remove capture device

        pthread_mutex_unlock(&m_capture_lock);
        pthread_mutex_unlock(&m_write_lock);
    }

    bool IoVideo::save(Image & image)
    {
        return true;
    }
    
    bool IoVideo::save(Image & image, JSON & data)
    {
        return true;
    }

    // -------------------------------------------
    // Function ran in a thread, which records for
    // a specific amount of time.
    
    void * recordContinuously(void * self)
    {
        IoVideo * video = (IoVideo *) self;
        
        double cronoPause = (double)cvGetTickCount();
        double cronoFPS = cronoPause;
        double cronoTime = 0;
        double timeElapsed = 0;
        double timeToSleep = 0;

        pthread_mutex_lock(&video->m_time_lock);
        double timeToRecord = video->m_timeStartedRecording + video->m_recordingTimeAfter;
        pthread_mutex_unlock(&video->m_time_lock);
        
        Image image = video->m_capture->retrieve();

        if(video->m_capture->m_angle != 0)
        {
            image.rotate(video->m_capture->m_angle);
        }
        // ---------------------
        // Apply mask if enabled

        if(video->m_privacy)
        {
            image.bitwiseAnd(video->m_mask, image);
        }

        // ------------------
        // Draw date on image
        
        video->drawDateOnImage(image, kerberos::helper::getTimestamp());
        
        pthread_mutex_lock(&video->m_lock);
        video->m_mostRecentImage = image;
        pthread_mutex_unlock(&video->m_lock);
        
        pthread_mutex_lock(&video->m_write_lock);
        while(cronoTime < timeToRecord)
        {
            cronoFPS = (double) cv::getTickCount();
            
            try
            {
                // -----------------------------
                // Write the frames to the video

                pthread_mutex_lock(&video->m_lock);
                Image image = video->m_mostRecentImage;
                video->m_writer->write(image.getImage());
                pthread_mutex_unlock(&video->m_lock);
            }
            catch(cv::Exception & ex)
            {
                pthread_mutex_unlock(&video->m_lock);
                pthread_mutex_destroy(&video->m_lock);
                LERROR << ex.what();
            }

            // update time to record; (locking)
            pthread_mutex_lock(&video->m_time_lock);
            timeToRecord = video->m_timeStartedRecording + video->m_recordingTimeAfter;
            pthread_mutex_unlock(&video->m_time_lock);
            
            cronoPause = (double) cv::getTickCount();
            cronoTime = cronoPause / cv::getTickFrequency();
            timeElapsed = (cronoPause - cronoFPS) / cv::getTickFrequency();
            double fpsToTime = 1. / video->m_fps;
            timeToSleep = fpsToTime - timeElapsed;
            
            if(timeToSleep > 0)
            {
                usleep(timeToSleep * 1000 * 1000);
            }
            else
            {
                LINFO << "IoVideo: framerate is too fast, can't record video at this speed (" << video->m_fps << "/FPS)";
            }
        }
        
        video->m_recording = false;
        video->m_writer->release();
        delete video->m_writer;
        video->m_writer = 0;
        
        if(video->m_createSymbol)
        {
            std::string link = SYMBOL_DIRECTORY + video->m_fileName;
            std::string pathToVideo = video->m_directory + video->m_fileName;
            symlink(pathToVideo.c_str(), link.c_str());
        }
        pthread_mutex_unlock(&video->m_write_lock);
    }
    
    void IoVideo::drawDateOnImage(Image & image, std::string timestamp)
    {
        if(m_drawTimestamp)
        {
            struct tm tstruct;
            char buf[80];
            
            time_t now = std::atoi(timestamp.c_str());
            
            char * timeformat = "%d-%m-%Y %X";
            if(m_timezone != "")
            {
                setenv("TZ", m_timezone.c_str(), 1);
                tzset();
            }
            
            tstruct = *localtime(&now);
            strftime(buf, sizeof(buf), timeformat, &tstruct);
            
            cv::putText(image.getImage(), buf, cv::Point(10,30), cv::FONT_HERSHEY_SIMPLEX, 0.5, getTimestampColor());
        }
    }

    // -------------------------------------------
    // Function ran in a thread, which records for
    // a specific amount of time.
    
    void * retrieveContinuously(void * self)
    {
        IoVideo * video = (IoVideo *) self;

        while(video->m_capture != 0)
        {
            if(video->m_recording)
            {
                pthread_mutex_lock(&video->m_capture_lock);
                
                try
                {
                    
                    if(video->m_capture != 0)
                    {
                        // -----------------------------
                        // Write the frames to the video
                        Image image = video->m_capture->retrieve();

                        if(video->m_capture->m_angle != 0)
                        {
                            image.rotate(video->m_capture->m_angle);
                        }

                        // ---------------------
                        // Apply mask if enabled

                        if(video->m_privacy)
                        {
                            image.bitwiseAnd(video->m_mask, image);
                        }

                        // ------------------
                        // Draw date on image
                        
                        video->drawDateOnImage(image, kerberos::helper::getTimestamp());
                    
                        pthread_mutex_lock(&video->m_lock);
                        video->m_mostRecentImage = image;
                        pthread_mutex_unlock(&video->m_lock);
                        usleep((int)(1000*1000/video->m_fps));
                    }
                }
                catch(cv::Exception & ex)
                {
                    pthread_mutex_unlock(&video->m_capture_lock);
                    pthread_mutex_destroy(&video->m_capture_lock);
                    pthread_mutex_unlock(&video->m_lock);
                    pthread_mutex_destroy(&video->m_lock);
                    throw OpenCVException(ex.msg.c_str());
                }
                
                pthread_mutex_unlock(&video->m_capture_lock);
            }
            else
            {
                usleep(500*1000);
            }
        }
    }
    
    void IoVideo::startRecordThread()
    {
        pthread_create(&m_recordThread, NULL, recordContinuously, this);
    }

    void IoVideo::stopRecordThread()
    {
        pthread_join(m_recordThread, NULL);
    }

    void IoVideo::startRetrieveThread()
    {
        pthread_create(&m_retrieveThread, NULL, retrieveContinuously, this);
    }
    
    void IoVideo::stopRetrieveThread()
    {
        pthread_join(m_recordThread, NULL);
    }
}