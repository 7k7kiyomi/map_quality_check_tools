#ifndef READSHP_CHECK_INTERFACE_H
#define READSHP_CHECK_INTERFACE_H

#include "ReadShp.h"

// ��̬�⵼����
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

// ͳһ������
typedef enum ReadShp_error {
    READSHP_SUCCESS = 0,
    READSHP_FAIL = 1,
    READSHP_INVALID_PARAM = 2,
    READSHP_FILE_NOT_FOUND = 3,
    READSHP_LAYER_LOAD_ERROR = 4,
    READSHP_GEOMETRY_INVALID = 5
} ReadShp_error;

#ifdef __cplusplus
extern "C" {
#endif

    // ========================= ͳһ�ӿڣ��������SHP + JSON���󱨸� =========================
    // ˵����
    // - szParsedShpFilePath�������Ľ���ͼ�㣨SHP/GeoPackage������Ϊͼ����Դ�ļ�·������
    // - szReferenceShpFilePath���ο�ͼ���ļ�·������ͼ������Ҫ����ͼ���鲻��Ҫ����
    // - szXmlConfigPath��XML�����ļ�·��;
    // - szOutputProblemShpFilePath������Ҫ������� Shapefile��.shp��Ŀ��·����
    // - szOutputJsonFilePath�����󱨸� JSON �����Ŀ��·����ʹ�� ReadShp.cpp ������ JSON ��ʽ����
   
    // - scale�������ߣ���������

    // ֱ����ֵ��飨��ͼ�㣩
    MY_PLUGIN_API ReadShp_error checkRightAngleThreshold(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // ���ص�/��϶��飨��ͼ�㣩
    MY_PLUGIN_API ReadShp_error checkPolygonOverlapGap(
        const char* szParsedShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // ��Ҫ�����˼��ͳһ�ӿڣ���ͼ�㣩
    // ˵����
    // - �����·�������ͼ���ļ�·������ʵ�ָ����ļ���/����Զ��ж��㷨��֧��
    // - ���������Ҫ�� Shapefile���͡�ReadShp.cpp ���и�ʽ�� JSON ���󱨸桱��
    // - ������־���������ߡ�
    MY_PLUGIN_API ReadShp_error checkLineTopology(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // 线要素拓扑检查：额外传入标准图层 CSV 和标准图层名称。
    MY_PLUGIN_API ReadShp_error checkLineTopologyWithStandardLayer(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szStandardLayerCsvPath,
        const char* szStandardLayerName,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // �����飨��ͼ�㣩
    MY_PLUGIN_API ReadShp_error checkWaterSystemDirection(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);
    // �����ӿڣ���ͼ�㣩����Ҫ�����˼��
    MY_PLUGIN_API ReadShp_error checkLineTopologyBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // �����ӿڣ���ͼ�㣩�����������쳣���
    MY_PLUGIN_API ReadShp_error checkWaterSystemDirectionBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // �����ӿڣ�һͼ�㣩����Ҫ���ص�/��϶���
    MY_PLUGIN_API ReadShp_error checkPolygonOverlapGapBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

    // �����ӿڣ���ͼ�㣩��ֱ�ǻ��Ƕ���ֵ����
    MY_PLUGIN_API ReadShp_error checkRightAngleThresholdBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale);

#ifdef __cplusplus
}
#endif

#endif // READSHP_CHECK_INTERFACE_H
