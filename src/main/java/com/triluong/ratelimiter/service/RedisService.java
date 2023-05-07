package com.triluong.ratelimiter.service;

import com.triluong.ratelimiter.model.Student;
import com.triluong.ratelimiter.repository.StudentRepository;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.List;

@Service
public class RedisService {

    @Autowired
    StudentRepository studentRepository;

    //Get a student from the id in the redis key store
    public Student getAStudent(String id) {
        return studentRepository.existsById(id) ? studentRepository.findById(id).get() : null;
    }

    //Write a student object into the redis key store
    public void writeStudent(Student student) {
        studentRepository.save(student);
    }

    //Updating an existing student record
    public void updateAStudent(String id, Student udpatedRecord) {
        Student foundStudent = studentRepository.findById(id).get();

        foundStudent.setId(udpatedRecord.getId());
        foundStudent.setName(udpatedRecord.getName());
        foundStudent.setGender(udpatedRecord.getGender());
        foundStudent.setGrade(udpatedRecord.getGrade());

    }


    //List all student record in the redis store
    public List<Student> listAllStudent() {
        List<Student> students = new ArrayList<>();
        studentRepository.findAll().forEach(students::add);

        return students;
    }

}
