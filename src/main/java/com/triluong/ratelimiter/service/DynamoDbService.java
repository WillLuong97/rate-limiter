package com.triluong.ratelimiter.service;

import com.amazonaws.services.dynamodbv2.AmazonDynamoDB;
import com.amazonaws.services.dynamodbv2.datamodeling.DynamoDBMapper;
import com.amazonaws.services.dynamodbv2.document.DynamoDB;
import com.amazonaws.services.dynamodbv2.document.Table;
import com.amazonaws.services.dynamodbv2.model.*;
import com.triluong.ratelimiter.repository.RequestInfoRepository;
import lombok.AllArgsConstructor;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

@Service
@RequiredArgsConstructor
@AllArgsConstructor
@Slf4j
public class DynamoDbService {

    //Adding the request repository service
    private final RequestInfoRepository requestInfoRepository;

    @Autowired
    private static AmazonDynamoDB amazonDynamoDB;

    @Autowired
    private static DynamoDB dynamoDB;

    @Autowired
    private static DynamoDBMapper dynamoDBMapper;


    /**
     *
     * Function to check if the table exist and set up the table in the database
     * if they have not been created.
     *
     * @param tableName - String -  the name of the table we are checking for
     *
     * **/
    public static void setUpTableIfRequired(String tableName) throws Exception {
        try {
            log.info("Setting up the DynamoDb Table...");
            //check to see if the table has been created or not,
            //if not, create a new ones
             if(Objects.isNull(amazonDynamoDB.describeTable(tableName))) {
                log.info("No tables with name {} found! Creating a new table...", tableName);
                createTable(tableName);
                log.info("Create table request went through! Validating new table exist...");

                TableDescription tableDescription = dynamoDB.getTable(tableName).describe();
                if (Objects.isNull(tableDescription)) {
                   throw new Exception("Table does not exist!");
                }
                log.info("Table with name {} successfully created", tableName);

             } else {
                 log.info("Table with name {} found! Skipping initial table set up!", tableName);
             }
        } catch (Exception e) {
            throw new Exception("Unable to verify table existence or create a new table! Error ", e);
        }
    }

    /**
     *
     * Function to create a table in the DynamoDb
     * @param tableName - String -  the name of the table that we want to create
     *
     *
     * **/
    public static void createTable(String tableName) throws InterruptedException {
        //To create a table, we must provide table name, primary key and provisioned throughput values,
        //The following code snipper creates an example table that uses the numeric type attribute ID as its
        //primary key.
        // reference - https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/JavaDocumentAPIWorkingWithTables.html
        List<AttributeDefinition> attributeDefinitions= new ArrayList<AttributeDefinition>();
        attributeDefinitions.add(new AttributeDefinition().withAttributeName("Id").withAttributeType("N"));

        List<KeySchemaElement> keySchema = new ArrayList<KeySchemaElement>();
        keySchema.add(new KeySchemaElement().withAttributeName("Id").withKeyType(KeyType.HASH));


        CreateTableRequest request = new CreateTableRequest()
                .withTableName(tableName)
                .withKeySchema(keySchema)
                .withAttributeDefinitions(attributeDefinitions)
                .withProvisionedThroughput(new ProvisionedThroughput()
                        .withReadCapacityUnits(5l)
                        .withWriteCapacityUnits(6l));

        //create a table;
        Table table = dynamoDB.createTable(request);

        //wait until table is created and active
        table.waitForActive();
    }
}
